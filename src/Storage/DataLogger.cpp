// Storage/DataLogger.cpp
// Portage Waveshare ESP32-S3-Relay-6CH
// Refactoring temps :
//  - push() utilise ManagerUTC::readUTC() pour PENDING et LastDataForWeb
//  - push() utilise VirtualClock::nowVirtual() pour LIVE (axe métier)
//  - handle() réparation via ManagerUTC::readUTC()
//  - tryFlush() sans garde RTCManager — flushe tout record UTC_available
//  - Format CSV 7 champs : timestamp,UTC_available,UTC_reliable,type,id,valueType,value
//  - LastDataForWeb toujours alimenté
//
// Refactoring META (source de vérité unique) :
//  - escapeCSV() et jsonEscape() centralisées comme méthodes publiques
//  - init() utilise findMetaIndex() au lieu de DataId::Count
//  - isValidId() remplace les tests manuels de bornes
#include "Storage/DataLogger.h"
#include "Connectivity/ManagerUTC.h"
#include "Core/VirtualClock.h"
#include "Config/TimingConfig.h"
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

// Callback publication (nullptr = pas de callback)
void (*DataLogger::_onPushCallback)(const DataRecord&) = nullptr;

void DataLogger::setOnPush(void (*callback)(const DataRecord&))
{
    _onPushCallback = callback;
}

// -----------------------------------------------------------------------------
// Utilitaires partagés — centralisés ici, déclarés publics dans DataLogger.h
// Utilisés par DataLogger, WebServer, MqttManager
// -----------------------------------------------------------------------------

// Échappe une String pour CSV : ajoute guillemets et double les guillemets internes
String DataLogger::escapeCSV(const String& text)
{
    String escaped = "\"";
    for (size_t i = 0; i < text.length(); i++) {
        char c = text.charAt(i);
        if (c == '"') {
            escaped += "\"\"";
        } else {
            escaped += c;
        }
    }
    escaped += "\"";
    return escaped;
}

// Échappement JSON minimal (caractères critiques uniquement)
// Les accents et caractères UTF-8 multi-octets sont valides en JSON sans escape.
String DataLogger::jsonEscape(const char* s)
{
    String out;
    if (!s) return out;
    out.reserve(strlen(s) + 4);
    while (*s) {
        char c = *s++;
        if      (c == '"')  { out += '\\'; out += '"';  }
        else if (c == '\\') { out += '\\'; out += '\\'; }
        else if (c == '\n') { out += '\\'; out += 'n';  }
        else if (c == '\r') { out += '\\'; out += 'r';  }
        else                { out += c; }
    }
    return out;
}

// -----------------------------------------------------------------------------
// Helpers CSV internes — parsing (utilisé uniquement dans ce fichier)
// -----------------------------------------------------------------------------

// Parse une String CSV (entre guillemets) et dé-échappe
// Entrée: "texte" ou "texte ""quoted"""
// Sortie: texte ou texte "quoted"
static String unescapeCSV(const String& text)
{
    String unescaped = "";

    if (text.length() < 2 || text.charAt(0) != '"' || text.charAt(text.length() - 1) != '"') {
        Console::warn(TAG, "CSV String sans guillemets: " + text);
        return text;
    }

    for (size_t i = 1; i < text.length() - 1; i++) {
        char c = text.charAt(i);
        if (c == '"') {
            if (i + 1 < text.length() - 1 && text.charAt(i + 1) == '"') {
                unescaped += '"';
                i++;
            } else {
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
    // Format CSV 7 champs : timestamp,UTC_available,UTC_reliable,type,id,valueType,value

    File file = SPIFFS.open("/datalog.csv", FILE_READ);
    if (!file) {
        // Fichier n'existe pas — normal au premier boot
        return;
    }

    // Table temporaire indexée par position dans META (pas par id)
    struct LastSeen {
        bool found = false;
        uint32_t timestamp = 0;
        bool UTC_available = false;
        bool UTC_reliable  = false;
        std::variant<float, String> value;
    };
    LastSeen lastSeen[META_COUNT];

    while (file.available()) {
        String line = file.readStringUntil('\n');
        if (line.length() == 0) continue;

        // Parser la ligne : timestamp,UTC_available,UTC_reliable,type,id,valueType,value
        int c1 = line.indexOf(',');
        int c2 = line.indexOf(',', c1 + 1);
        int c3 = line.indexOf(',', c2 + 1);
        int c4 = line.indexOf(',', c3 + 1);
        int c5 = line.indexOf(',', c4 + 1);
        int c6 = line.indexOf(',', c5 + 1);

        if (c1 == -1 || c2 == -1 || c3 == -1 || c4 == -1 || c5 == -1 || c6 == -1) {
            continue;  // Ligne mal formatée, ignorer
        }

        unsigned long ts     = line.substring(0, c1).toInt();
        uint8_t avail        = line.substring(c1 + 1, c2).toInt();
        uint8_t reliable     = line.substring(c2 + 1, c3).toInt();
        // typeByte (c3→c4) ignoré : META est la source de vérité pour le type
        uint8_t idByte       = line.substring(c4 + 1, c5).toInt();
        uint8_t valueType    = line.substring(c5 + 1, c6).toInt();
        String valueStr      = line.substring(c6 + 1);

        // Recherche de l'index dans META pour cet id
        int metaIdx = findMetaIndex(idByte);
        if (metaIdx < 0) continue;  // Id inconnu dans META, ignorer

        LastSeen& ls = lastSeen[metaIdx];
        ls.found         = true;
        ls.timestamp     = ts;
        ls.UTC_available = (avail != 0);
        ls.UTC_reliable  = (reliable != 0);

        if (valueType == 0) {
            ls.value = valueStr.toFloat();
        } else {
            valueStr.trim();
            ls.value = unescapeCSV(valueStr);
        }
    }

    file.close();

    // Peupler lastDataForWeb depuis la table temporaire
    for (size_t m = 0; m < META_COUNT; m++) {
        if (lastSeen[m].found) {
            LastDataForWeb e;
            e.value         = lastSeen[m].value;
            e.timestamp     = lastSeen[m].timestamp;
            e.UTC_available = lastSeen[m].UTC_available;
            e.UTC_reliable  = lastSeen[m].UTC_reliable;
            lastDataForWeb[META[m].id] = e;
        }
    }
}

// -----------------------------------------------------------------------------
// PUSH — point d'entrée pour valeurs NUMÉRIQUES (float)
// DataType déduit automatiquement de META (source de vérité unique)
// -----------------------------------------------------------------------------
void DataLogger::push(DataId id, float value)
{
    DataType type = getMeta(id).type;
    TimeUTC t = ManagerUTC::readUTC();

    // LIVE — VirtualClock (axe métier, toujours un temps absolu)
    DataRecord liveRec;
    liveRec.type          = type;
    liveRec.id            = id;
    liveRec.value         = value;
    liveRec.timestamp     = static_cast<uint32_t>(VirtualClock::nowVirtual());
    liveRec.UTC_available = true;
    liveRec.UTC_reliable  = false;
    addLive(liveRec);

    // PENDING — readUTC() fournit toujours un temps
    DataRecord pendRec;
    pendRec.type          = type;
    pendRec.id            = id;
    pendRec.value         = value;
    pendRec.timestamp     = static_cast<uint32_t>(t.timestamp);
    pendRec.UTC_available = t.UTC_available;
    pendRec.UTC_reliable  = t.UTC_reliable;
    addPending(pendRec);

    // Vue Web — toujours alimenté
    LastDataForWeb& w = lastDataForWeb[id];
    w.value         = value;
    w.timestamp     = t.timestamp;
    w.UTC_available = t.UTC_available;
    w.UTC_reliable  = t.UTC_reliable;

    // Notification publication (MQTT ou autre)
    if (_onPushCallback) _onPushCallback(pendRec);
}

// -----------------------------------------------------------------------------
// PUSH — point d'entrée pour valeurs TEXTUELLES (String)
// DataType déduit automatiquement de META (source de vérité unique)
// -----------------------------------------------------------------------------
void DataLogger::push(DataId id, const String& textValue)
{
    DataType type = getMeta(id).type;
    TimeUTC t = ManagerUTC::readUTC();

    // LIVE — VirtualClock (axe métier)
    DataRecord liveRec;
    liveRec.type          = type;
    liveRec.id            = id;
    liveRec.value         = textValue;
    liveRec.timestamp     = static_cast<uint32_t>(VirtualClock::nowVirtual());
    liveRec.UTC_available = true;
    liveRec.UTC_reliable  = false;
    addLive(liveRec);

    // PENDING — readUTC() fournit toujours un temps
    DataRecord pendRec;
    pendRec.type          = type;
    pendRec.id            = id;
    pendRec.value         = textValue;
    pendRec.timestamp     = static_cast<uint32_t>(t.timestamp);
    pendRec.UTC_available = t.UTC_available;
    pendRec.UTC_reliable  = t.UTC_reliable;
    addPending(pendRec);

    // Vue Web — toujours alimenté
    LastDataForWeb& w = lastDataForWeb[id];
    w.value         = textValue;
    w.timestamp     = t.timestamp;
    w.UTC_available = t.UTC_available;
    w.UTC_reliable  = t.UTC_reliable;

    // Notification publication (MQTT ou autre)
    if (_onPushCallback) _onPushCallback(pendRec);
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
    TimeUTC t = ManagerUTC::readUTC();
    if (t.UTC_available) {
        for (size_t i = 0; i < pendingCount; ++i) {
            size_t idx = (pendingHead + i) % PENDING_SIZE;
            if (!pending[idx].UTC_available) {
                int32_t deltaMs = static_cast<int32_t>(millis() - pending[idx].timestamp);
                time_t repaired = t.timestamp - static_cast<time_t>(deltaMs / 1000L);

                if (repaired > 0) {
                    pending[idx].timestamp     = static_cast<uint32_t>(repaired);
                    pending[idx].UTC_available = true;
                    pending[idx].UTC_reliable  = t.UTC_reliable;
                }
            }
        }
    }

    bool flushByCount =
        pendingCount >= FLUSH_SIZE;

    bool flushByTime = false;
    if (pendingCount > 0 && millis() - lastFlushMs >= FLUSH_HOURLY_MIN_INTERVAL_MS) {
        if (t.UTC_available) {
            uint32_t secInHour = static_cast<uint32_t>(t.timestamp) % 3600;
            flushByTime = (secInHour < FLUSH_HOURLY_WINDOW_SEC);
        } else {
            flushByTime = (millis() - lastFlushMs >= FLUSH_TIMEOUT_MS);
        }
    }

    if (flushByCount || flushByTime) {
        tryFlush();
    }
}

// -----------------------------------------------------------------------------
// TRY FLUSH — flushe les records UTC_available contigus depuis la tête
// -----------------------------------------------------------------------------
void DataLogger::tryFlush()
{
    size_t flushable = 0;
    for (size_t i = 0; i < pendingCount; ++i) {
        size_t idx = (pendingHead + i) % PENDING_SIZE;
        if (pending[idx].UTC_available) {
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
// Format CSV : timestamp,UTC_available,UTC_reliable,type,id,valueType,value
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

        if (std::holds_alternative<float>(r.value)) {
            float val = std::get<float>(r.value);
            f.printf("%lu,%d,%d,%d,%d,0,%.3f\n",
                     r.timestamp,
                     (int)r.UTC_available,
                     (int)r.UTC_reliable,
                     (int)r.type,
                     (int)r.id,
                     val);
        } else {
            String txt = std::get<String>(r.value);
            String escaped = escapeCSV(txt);
            f.printf("%lu,%d,%d,%d,%d,1,%s\n",
                     r.timestamp,
                     (int)r.UTC_available,
                     (int)r.UTC_reliable,
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

    if (SPIFFS.remove("/datalog.csv")) {
        Console::info(TAG, "Fichier /datalog.csv supprimé avec succès");
    } else {
        Console::warn(TAG, "Impossible de supprimer /datalog.csv (peut-être inexistant)");
    }

    pendingHead = 0;
    pendingCount = 0;

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
// -----------------------------------------------------------------------------
LogFileStats DataLogger::getLogFileStats()
{
    LogFileStats stats;
    stats.exists = false;
    stats.sizeBytes = 0;
    stats.sizeMB = 0.0f;
    stats.percentFull = 0.0f;
    stats.totalMB = 2.0f;

    File file = SPIFFS.open("/datalog.csv", FILE_READ);
    if (!file) {
        return stats;
    }

    stats.exists = true;
    stats.sizeBytes = file.size();
    stats.sizeMB = stats.sizeBytes / (1024.0f * 1024.0f);

    file.close();

    stats.percentFull = (stats.sizeMB / stats.totalMB) * 100.0f;

    Console::debug(TAG, "Stats fichier: " + String(stats.sizeMB, 2)
                   + " MB (" + String(stats.percentFull, 1)
                   + "% de " + String(stats.totalMB, 1) + " MB)");

    return stats;
}

// -----------------------------------------------------------------------------
// FLASH — dernière valeur UTC
// Format CSV : timestamp,UTC_available,UTC_reliable,type,id,valueType,value
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

        int c1 = line.indexOf(',');
        int c2 = line.indexOf(',', c1 + 1);
        int c3 = line.indexOf(',', c2 + 1);
        int c4 = line.indexOf(',', c3 + 1);
        int c5 = line.indexOf(',', c4 + 1);
        int c6 = line.indexOf(',', c5 + 1);

        if (c1 == -1 || c2 == -1 || c3 == -1 || c4 == -1 || c5 == -1 || c6 == -1) {
            Console::warn(TAG, "Ligne CSV mal formatée (virgules manquantes): " + line);
            continue;
        }

        uint8_t idByte = line.substring(c4 + 1, c5).toInt();

        if (idByte == static_cast<uint8_t>(id)) {
            candidate.timestamp     = line.substring(0, c1).toInt();
            candidate.UTC_available = (line.substring(c1 + 1, c2).toInt() != 0);
            candidate.UTC_reliable  = (line.substring(c2 + 1, c3).toInt() != 0);
            // typeByte (c3→c4) ignoré : META est la source de vérité pour le type
            candidate.type          = getMeta(id).type;
            candidate.id            = id;

            uint8_t valueType = line.substring(c5 + 1, c6).toInt();
            String valueStr   = line.substring(c6 + 1);

            if (valueType == 0) {
                candidate.value = valueStr.toFloat();
            } else {
                valueStr.trim();
                candidate.value = unescapeCSV(valueStr);
            }
            found = true;
        }
    }

    file.close();
    if (found) {
        out = candidate;
    }
    return found;
}

// -----------------------------------------------------------------------------
// GRAPH CSV (FLASH) — dernières mesures numériques
// Format CSV : timestamp,UTC_available,UTC_reliable,type,id,valueType,value
// -----------------------------------------------------------------------------
String DataLogger::getGraphCsv(DataId id, uint32_t maxPoints)
{
    File file = SPIFFS.open("/datalog.csv", FILE_READ);
    if (!file) {
        Console::error(TAG, "Cannot open /datalog.csv for reading (getGraphCsv)");
        return "";
    }

    struct GraphPoint {
        uint32_t ts;
        float    val;
    };

    GraphPoint* buf = new (std::nothrow) GraphPoint[maxPoints];
    if (!buf) {
        file.close();
        Console::error(TAG, "getGraphCsv: allocation buffer échouée");
        return "";
    }

    size_t bufHead  = 0;
    size_t bufCount = 0;

    while (file.available()) {
        String line = file.readStringUntil('\n');
        if (line.length() == 0) continue;

        int c1 = line.indexOf(',');
        int c2 = line.indexOf(',', c1 + 1);
        int c3 = line.indexOf(',', c2 + 1);
        int c4 = line.indexOf(',', c3 + 1);
        int c5 = line.indexOf(',', c4 + 1);
        int c6 = line.indexOf(',', c5 + 1);

        if (c1 == -1 || c2 == -1 || c3 == -1 || c4 == -1 || c5 == -1 || c6 == -1) {
            continue;
        }

        uint8_t idByte    = line.substring(c4 + 1, c5).toInt();
        uint8_t valueType = line.substring(c5 + 1, c6).toInt();

        if (idByte == static_cast<uint8_t>(id) && valueType == 0) {
            uint32_t ts = line.substring(0, c1).toInt();
            float val   = line.substring(c6 + 1).toFloat();

            buf[bufHead].ts  = ts;
            buf[bufHead].val = val;
            bufHead = (bufHead + 1) % maxPoints;
            if (bufCount < maxPoints) bufCount++;
        }
    }

    file.close();

    String csv = "timestamp,value\n";
    size_t start = (bufCount < maxPoints) ? 0 : bufHead;

    for (size_t i = 0; i < bufCount; i++) {
        size_t idx = (start + i) % maxPoints;
        csv += String(buf[idx].ts) + ",";
        csv += String(buf[idx].val, 2) + "\n";
    }

    delete[] buf;

    Console::debug(TAG, "getGraphCsv: " + String(bufCount)
                   + " points pour DataId " + String((int)id));

    return csv;
}