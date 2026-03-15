// Storage/DataLogger.cpp
// Portage Waveshare ESP32-S3-Relay-6CH
// Changement : correction getLogFileStats() — totalGB → totalMB (partition SPIFFS = 2 MB)
// Changement : Serial.println/printf → Console::info/warn/error/debug
#include "Storage/DataLogger.h"
#include "Connectivity/ManagerUTC.h"
#include "Utils/Console.h"
#include <SPIFFS.h>
#include <time.h>

// Tag pour logs Console
static const char* TAG = "DataLogger";

// -----------------------------------------------------------------------------
// Buffers
// -----------------------------------------------------------------------------
DataRecord DataLogger::live[LIVE_SIZE];
DataRecord DataLogger::pending[PENDING_SIZE];

size_t DataLogger::liveIndex    = 0;

// Pending FIFO circulaire
size_t DataLogger::pendingHead  = 0;   // index du plus ancien
size_t DataLogger::pendingCount = 0;   // nombre d'éléments valides

std::map<DataId, LastDataForWeb> DataLogger::lastDataForWeb;

static unsigned long lastFlushMs = 0;

// -----------------------------------------------------------------------------
// Helpers CSV - Échappement et parsing
// -----------------------------------------------------------------------------

// Échappe une String pour CSV : ajoute guillemets et double les guillemets internes
static String escapeCSV(const String& text)
{
    String escaped = "\"";  // Commence avec un guillemet
    
    for (size_t i = 0; i < text.length(); i++) {
        char c = text.charAt(i);
        if (c == '"') {
            escaped += "\"\"";  // Double les guillemets
        } else {
            escaped += c;
        }
    }
    
    escaped += "\"";  // Termine avec un guillemet
    return escaped;
}

// Parse une String CSV (entre guillemets) et dé-échappe
// Entrée: "texte" ou "texte ""quoted""" 
// Sortie: texte ou texte "quoted"
static String unescapeCSV(const String& text)
{
    String unescaped = "";
    
    // Vérifier que la String commence et finit par des guillemets
    if (text.length() < 2 || text.charAt(0) != '"' || text.charAt(text.length() - 1) != '"') {
        // Pas de guillemets = format invalide, retourner tel quel
        Console::warn(TAG, "CSV String sans guillemets: " + text);
        return text;
    }
    
    // Parser le contenu entre les guillemets
    for (size_t i = 1; i < text.length() - 1; i++) {
        char c = text.charAt(i);
        if (c == '"') {
            // Vérifier si c'est un guillemet doublé
            if (i + 1 < text.length() - 1 && text.charAt(i + 1) == '"') {
                unescaped += '"';  // Ajouter un seul guillemet
                i++;  // Sauter le deuxième guillemet
            } else {
                // Guillemet seul = erreur de format
                Console::warn(TAG, "Guillemet non échappé dans CSV");
                unescaped += c;
            }
        } else {
            unescaped += c;
        }
    }
    
    return unescaped;
}

// -----------------------------------------------------------------------------
// Temps
// -----------------------------------------------------------------------------
uint32_t DataLogger::nowRelative()
{
    return millis();
}

// -----------------------------------------------------------------------------
// Initialisation
// -----------------------------------------------------------------------------
void DataLogger::init()
{
    lastFlushMs = millis();

    pendingHead  = 0;
    pendingCount = 0;

    // Reconstruction LastDataForWeb depuis la flash
    // LECTURE UNIQUE du fichier CSV : on parcourt toutes les lignes
    // et on garde la dernière valeur rencontrée pour chaque DataId.
    // (Avant : 1 lecture complète par DataId = 22 lectures → ~60s sur SPIFFS)

    File file = SPIFFS.open("/datalog.csv", FILE_READ);
    if (!file) {
        // Fichier n'existe pas — normal au premier boot
        return;
    }

    // Table temporaire : dernière ligne vue pour chaque DataId
    struct LastSeen {
        bool found = false;
        uint32_t timestamp = 0;
        DataType type = DataType::System;
        std::variant<float, String> value;
    };
    LastSeen lastSeen[(int)DataId::Count];

    while (file.available()) {
        String line = file.readStringUntil('\n');
        if (line.length() == 0) continue;

        // Parser la ligne : timestamp,type,id,valueType,value
        int firstComma = line.indexOf(',');
        int secondComma = line.indexOf(',', firstComma + 1);
        int thirdComma = line.indexOf(',', secondComma + 1);
        int fourthComma = line.indexOf(',', thirdComma + 1);

        if (firstComma == -1 || secondComma == -1 || thirdComma == -1 || fourthComma == -1) {
            continue;  // Ligne mal formatée, ignorer
        }

        unsigned long ts = line.substring(0, firstComma).toInt();
        uint8_t typeByte = line.substring(firstComma + 1, secondComma).toInt();
        uint8_t idByte = line.substring(secondComma + 1, thirdComma).toInt();
        uint8_t valueType = line.substring(thirdComma + 1, fourthComma).toInt();
        String valueStr = line.substring(fourthComma + 1);

        if (idByte >= (uint8_t)DataId::Count) continue;  // Id hors limites

        LastSeen& ls = lastSeen[idByte];
        ls.found = true;
        ls.timestamp = ts;
        ls.type = static_cast<DataType>(typeByte);

        if (valueType == 0) {
            ls.value = valueStr.toFloat();
        } else {
            valueStr.trim();
            ls.value = unescapeCSV(valueStr);
        }
    }

    file.close();

    // Peupler lastDataForWeb depuis la table temporaire
    for (int id = 0; id < (int)DataId::Count; ++id) {
        if (lastSeen[id].found) {
            LastDataForWeb e;
            e.value     = lastSeen[id].value;
            e.t_rel_ms  = 0;
            e.t_utc     = lastSeen[id].timestamp;
            e.utc_valid = true;
            lastDataForWeb[(DataId)id] = e;
        }
    }
}

// -----------------------------------------------------------------------------
// PUSH — point d'entrée pour valeurs NUMÉRIQUES (float)
// -----------------------------------------------------------------------------
void DataLogger::push(DataType type, DataId id, float value)
{
    uint32_t relNow = nowRelative();
    bool utcValid   = ManagerUTC::isUtcValid();
    uint32_t utcNow = utcValid ? ManagerUTC::nowUtc() : 0;

    // LIVE (toujours relatif)
    DataRecord liveRec;
    liveRec.type      = type;
    liveRec.id        = id;
    liveRec.value     = value;  // std::variant accepte float
    liveRec.timestamp = relNow;
    liveRec.timeBase  = TimeBase::Relative;
    addLive(liveRec);

    // PENDING
    DataRecord pendRec;
    pendRec.type      = type;
    pendRec.id        = id;
    pendRec.value     = value;  // std::variant accepte float
    pendRec.timestamp = utcValid ? utcNow : relNow;
    pendRec.timeBase  = utcValid ? TimeBase::UTC : TimeBase::Relative;
    addPending(pendRec);

    // Vue Web
    LastDataForWeb& w = lastDataForWeb[id];
    w.value = value;  // std::variant accepte float

    if (utcValid) {
        w.t_utc     = utcNow;
        w.utc_valid = true;
    } else {
        w.t_rel_ms  = relNow;
        w.utc_valid = false;
    }
}

// -----------------------------------------------------------------------------
// PUSH — point d'entrée pour valeurs TEXTUELLES (String)
// -----------------------------------------------------------------------------
void DataLogger::push(DataType type, DataId id, const String& textValue)
{
    uint32_t relNow = nowRelative();
    bool utcValid   = ManagerUTC::isUtcValid();
    uint32_t utcNow = utcValid ? ManagerUTC::nowUtc() : 0;

    // LIVE (toujours relatif)
    DataRecord liveRec;
    liveRec.type      = type;
    liveRec.id        = id;
    liveRec.value     = textValue;  // std::variant accepte String
    liveRec.timestamp = relNow;
    liveRec.timeBase  = TimeBase::Relative;
    addLive(liveRec);

    // PENDING
    DataRecord pendRec;
    pendRec.type      = type;
    pendRec.id        = id;
    pendRec.value     = textValue;  // std::variant accepte String
    pendRec.timestamp = utcValid ? utcNow : relNow;
    pendRec.timeBase  = utcValid ? TimeBase::UTC : TimeBase::Relative;
    addPending(pendRec);

    // Vue Web - stocke le String dans le variant
    LastDataForWeb& w = lastDataForWeb[id];
    w.value = textValue;  // std::variant accepte String

    if (utcValid) {
        w.t_utc     = utcNow;
        w.utc_valid = true;
    } else {
        w.t_rel_ms  = relNow;
        w.utc_valid = false;
    }
}

// -----------------------------------------------------------------------------
// LIVE
// -----------------------------------------------------------------------------
void DataLogger::addLive(const DataRecord& r)
{
    live[liveIndex] = r;
    liveIndex = (liveIndex + 1) % LIVE_SIZE;
}

// -----------------------------------------------------------------------------
// PENDING — FIFO circulaire avec perte FIFO
// -----------------------------------------------------------------------------
void DataLogger::addPending(const DataRecord& r)
{
    // Si plein : on perd le plus ancien (FIFO)
    if (pendingCount == PENDING_SIZE) {
        pendingHead = (pendingHead + 1) % PENDING_SIZE;
        pendingCount--;
    }

    size_t index =
        (pendingHead + pendingCount) % PENDING_SIZE;

    pending[index] = r;
    pendingCount++;
}

// -----------------------------------------------------------------------------
// HANDLE — réparation + flush
// -----------------------------------------------------------------------------
void DataLogger::handle()
{
    // Réparation UTC si NTP devenu valide
    if (ManagerUTC::isUtcValid()) {
        for (size_t i = 0; i < pendingCount; ++i) {
            size_t idx = (pendingHead + i) % PENDING_SIZE;
            if (pending[idx].timeBase == TimeBase::Relative) {
                pending[idx].timestamp =
                    ManagerUTC::convertFromRelative(pending[idx].timestamp);
                pending[idx].timeBase = TimeBase::UTC;
            }
        }
    }

    bool flushByCount =
        pendingCount >= FLUSH_SIZE;

    bool flushByTime =
        pendingCount > 0 &&
        (millis() - lastFlushMs >= FLUSH_TIMEOUT_MS);

    if (flushByCount || flushByTime) {
        tryFlush();
    }
}

// -----------------------------------------------------------------------------
// TRY FLUSH
// -----------------------------------------------------------------------------
void DataLogger::tryFlush()
{
    if (!ManagerUTC::isUtcValid()) return;

    size_t flushable = 0;
    for (size_t i = 0; i < pendingCount; ++i) {
        size_t idx = (pendingHead + i) % PENDING_SIZE;
        if (pending[idx].timeBase == TimeBase::UTC) {
            flushable++;
        } else {
            break;
        }
    }

    if (flushable == 0) return;

    size_t toFlush = min(flushable, FLUSH_SIZE);
    flushToFlash(toFlush);
}

// -----------------------------------------------------------------------------
// FLUSH TO FLASH
// Format CSV : timestamp,type,id,valueType,value
// valueType = 0 pour float, 1 pour String
// -----------------------------------------------------------------------------
void DataLogger::flushToFlash(size_t count)
{
    File f = SPIFFS.open("/datalog.csv", FILE_APPEND);
    if (!f) {
        Console::error(TAG, "Cannot open /datalog.csv for writing");
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        size_t idx = (pendingHead + i) % PENDING_SIZE;
        DataRecord& r = pending[idx];

        // Déterminer le type de valeur et l'écrire
        if (std::holds_alternative<float>(r.value)) {
            // Valeur numérique
            float val = std::get<float>(r.value);
            f.printf("%lu,%d,%d,0,%.3f\n",
                     r.timestamp,
                     (int)r.type,
                     (int)r.id,
                     val);
        } else {
            // Valeur textuelle - ÉCHAPPER avec guillemets CSV
            String txt = std::get<String>(r.value);
            String escaped = escapeCSV(txt);
            f.printf("%lu,%d,%d,1,%s\n",
                     r.timestamp,
                     (int)r.type,
                     (int)r.id,
                     escaped.c_str());
        }
    }
    f.close();

    pendingHead =
        (pendingHead + count) % PENDING_SIZE;
    pendingCount -= count;

    lastFlushMs = millis();
}

// -----------------------------------------------------------------------------
// CLEAR HISTORY - Suppression historique et réinitialisation
// -----------------------------------------------------------------------------
void DataLogger::clearHistory()
{
    Console::info(TAG, "Suppression de l'historique...");
    
    // Supprimer le fichier CSV
    if (SPIFFS.remove("/datalog.csv")) {
        Console::info(TAG, "Fichier /datalog.csv supprimé avec succès");
    } else {
        Console::warn(TAG, "Impossible de supprimer /datalog.csv (peut-être inexistant)");
    }
    
    // Réinitialiser les buffers PENDING (Option A : on garde lastDataForWeb)
    pendingHead = 0;
    pendingCount = 0;
    
    // Note: lastDataForWeb n'est PAS vidé - on garde les dernières valeurs en RAM
    // pour continuer à afficher les données actuelles sur l'interface web
    
    Console::info(TAG, "Buffers réinitialisés. Historique vidé.");
}

// -----------------------------------------------------------------------------
// WEB — dernière valeur RAM
// -----------------------------------------------------------------------------
bool DataLogger::hasLastDataForWeb(DataId id, LastDataForWeb& out)
{
    auto it = lastDataForWeb.find(id);
    if (it == lastDataForWeb.end()) return false;
    out = it->second;
    return true;
}

// -----------------------------------------------------------------------------
// STATISTIQUES FICHIER DE LOGS
// Correction : partition SPIFFS = 2 MB (et non 1.9 Go)
// -----------------------------------------------------------------------------
LogFileStats DataLogger::getLogFileStats()
{
    LogFileStats stats;
    stats.exists = false;
    stats.sizeBytes = 0;
    stats.sizeMB = 0.0f;
    stats.percentFull = 0.0f;
    stats.totalMB = 2.0f;  // Partition SPIFFS : 2 MB (0x200000)
    
    File file = SPIFFS.open("/datalog.csv", FILE_READ);
    if (!file) {
        // Fichier n'existe pas - c'est normal
        return stats;
    }
    
    stats.exists = true;
    stats.sizeBytes = file.size();
    stats.sizeMB = stats.sizeBytes / (1024.0f * 1024.0f);
    
    file.close();
    
    // Calcul du pourcentage sur 2 MB
    stats.percentFull = (stats.sizeMB / stats.totalMB) * 100.0f;
    
    Console::debug(TAG, "Stats fichier: " + String(stats.sizeMB, 2)
                   + " MB (" + String(stats.percentFull, 1)
                   + "% de " + String(stats.totalMB, 1) + " MB)");
    
    return stats;
}

// -----------------------------------------------------------------------------
// FLASH — dernière valeur UTC
// Format CSV : timestamp,type,id,valueType,value
// -----------------------------------------------------------------------------
bool DataLogger::getLastUtcRecord(DataId id, DataRecord& out)
{
    File file = SPIFFS.open("/datalog.csv", FILE_READ);
    if (!file) {
        Console::error(TAG, "Cannot open /datalog.csv for reading");
        return false;
    }

    String line;
    bool found = false;
    DataRecord candidate;

    while (file.available()) {
        line = file.readStringUntil('\n');
        if (line.length() == 0) continue;

        unsigned long ts;
        uint8_t typeByte, idByte, valueType;
        
        // Parser la ligne selon le format : timestamp,type,id,valueType,value
        int firstComma = line.indexOf(',');
        int secondComma = line.indexOf(',', firstComma + 1);
        int thirdComma = line.indexOf(',', secondComma + 1);
        int fourthComma = line.indexOf(',', thirdComma + 1);
        
        if (firstComma == -1 || secondComma == -1 || thirdComma == -1 || fourthComma == -1) {
            Console::warn(TAG, "Ligne CSV mal formatée (virgules manquantes): " + line);
            continue; // Format invalide
        }
        
        ts = line.substring(0, firstComma).toInt();
        typeByte = line.substring(firstComma + 1, secondComma).toInt();
        idByte = line.substring(secondComma + 1, thirdComma).toInt();
        valueType = line.substring(thirdComma + 1, fourthComma).toInt();
        String valueStr = line.substring(fourthComma + 1);
        
        if (idByte == static_cast<uint8_t>(id)) {
            candidate.timestamp = ts;
            candidate.timeBase  = TimeBase::UTC;
            candidate.type      = static_cast<DataType>(typeByte);
            candidate.id        = id;
            
            if (valueType == 0) {
                // Valeur numérique
                candidate.value = valueStr.toFloat();
            } else {
                // Valeur textuelle - DÉSECHAPPER
                valueStr.trim(); // Enlever les espaces/retours à la ligne
                candidate.value = unescapeCSV(valueStr);
            }
            found = true;
        }
    }

    file.close();
    if (found) {
        out = candidate;
    }
    // PAS de log si pas trouvé - c'est normal
    return found;
}

// -----------------------------------------------------------------------------
// GRAPH CSV (FLASH) — avec timestamp UTC
// ATTENTION : Ne fonctionne que pour les valeurs NUMÉRIQUES
// -----------------------------------------------------------------------------
String DataLogger::getGraphCsv(DataId id, uint32_t daysBack)
{
    File file = SPIFFS.open("/datalog.csv", FILE_READ);
    if (!file) {
        Console::error(TAG, "Cannot open /datalog.csv for reading (getGraphCsv)");
        return "";
    }

    uint32_t cutoffTime = 0;
    if (daysBack > 0) {
        cutoffTime = ManagerUTC::nowUtc() - (daysBack * 86400UL);
    }

    String csv = "timestamp,value\n";
    int validLines = 0;

    while (file.available()) {
        String line = file.readStringUntil('\n');
        if (line.length() == 0) continue;

        unsigned long ts;
        uint8_t typeByte, idByte, valueType;
        
        // Parser la ligne
        int firstComma = line.indexOf(',');
        int secondComma = line.indexOf(',', firstComma + 1);
        int thirdComma = line.indexOf(',', secondComma + 1);
        int fourthComma = line.indexOf(',', thirdComma + 1);
        
        if (firstComma == -1 || secondComma == -1 || thirdComma == -1 || fourthComma == -1) {
            continue; // Format invalide - ignorer silencieusement
        }
        
        ts = line.substring(0, firstComma).toInt();
        typeByte = line.substring(firstComma + 1, secondComma).toInt();
        idByte = line.substring(secondComma + 1, thirdComma).toInt();
        valueType = line.substring(thirdComma + 1, fourthComma).toInt();
        String valueStr = line.substring(fourthComma + 1);

        // Ne traiter que les valeurs numériques (valueType == 0)
        if (idByte == static_cast<uint8_t>(id) &&
            valueType == 0 &&
            (daysBack == 0 || ts >= cutoffTime))
        {
            float val = valueStr.toFloat();
            csv += String(ts) + ",";
            csv += String(val, 2) + "\n";
            validLines++;
        }
    }

    file.close();
    
    Console::debug(TAG, "getGraphCsv: " + String(validLines)
                   + " lignes pour DataId " + String((int)id));
    
    return csv;
}