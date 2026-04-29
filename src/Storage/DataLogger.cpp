// Storage/DataLogger.cpp
// Logger LittleFS — double buffer + rotation quotidienne.
//
// handle() est appelé toutes les 3 secondes par TaskManager.
// À chaque appel :
//   1. Drain DataBus::logQueue → PENDING
//   2. Réparation timestamps millis() → UTC (si VClock disponible)
//   3. Transfert d'UN SEUL record réparé → buffer CSV actif
//   4. Si buffer actif plein (14 records ou 512 octets) → swap + flush
//   5. Vérification rotation quotidienne
//
// Écriture flash : un seul file.write() + file.flush() + yield() par chunk.
// Fichier ouvert en permanence en mode append, rotation par date.

#include "Storage/DataLogger.h"
#include "Core/DataBus.h"
#include "Core/VirtualClock.h"
#include "Config/TimingConfig.h"
#include "Utils/Console.h"
#include <LittleFS.h>
#include <time.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>

static const char* TAG = "DataLogger";

// -----------------------------------------------------------------------------
// Variables statiques — PENDING
// -----------------------------------------------------------------------------
DataRecord DataLogger::pending[PENDING_SIZE];
size_t DataLogger::pendingHead  = 0;
size_t DataLogger::pendingCount = 0;

// -----------------------------------------------------------------------------
// Variables statiques — Double buffer
// -----------------------------------------------------------------------------
char   DataLogger::bufferA[CHUNK_BUFFER_SIZE];
char   DataLogger::bufferB[CHUNK_BUFFER_SIZE];
char*  DataLogger::activeBuffer = bufferA;
char*  DataLogger::flushBuffer  = bufferB;
size_t DataLogger::activeLen    = 0;
size_t DataLogger::activeCount  = 0;
size_t DataLogger::flushLen     = 0;

// -----------------------------------------------------------------------------
// Variables statiques — Fichier et rotation
// -----------------------------------------------------------------------------
File DataLogger::logFile;
bool DataLogger::logFileOpen     = false;
char DataLogger::logFilePath[32] = {0};
int  DataLogger::lastRotationDate = -1;

// Constante locale : minute de rotation dans la journée
static constexpr int ROTATION_MINUTE = DATALOGGER_ROTATION_HOUR * 60
                                     + DATALOGGER_ROTATION_MINUTE;

// -----------------------------------------------------------------------------
// Initialisation
// -----------------------------------------------------------------------------
void DataLogger::init()
{
    pendingHead  = 0;
    pendingCount = 0;

    activeBuffer = bufferA;
    flushBuffer  = bufferB;
    activeLen    = 0;
    activeCount  = 0;
    flushLen     = 0;

    logFileOpen       = false;
    logFilePath[0]    = '\0';
    lastRotationDate  = -1;

    logFlashUsage();
}

// -----------------------------------------------------------------------------
// Utilitaires date/heure locale
// -----------------------------------------------------------------------------
int DataLogger::localDate(time_t utc)
{
    struct tm local;
    localtime_r(&utc, &local);
    return (local.tm_year + 1900) * 10000
         + (local.tm_mon + 1) * 100
         +  local.tm_mday;
}

int DataLogger::localMinuteOfDay(time_t utc)
{
    struct tm local;
    localtime_r(&utc, &local);
    return local.tm_hour * 60 + local.tm_min;
}

// -----------------------------------------------------------------------------
// Construction du chemin de fichier log
//
// Convention : le fichier est nommé d'après la date à laquelle sa période
// commence. Une période commence à l'heure de rotation (ex. 16:15).
//   - Avant 16:15 → le fichier courant a été ouvert hier → date = veille
//   - Après 16:15 → le fichier courant a été ouvert aujourd'hui → date = jour
// -----------------------------------------------------------------------------
void DataLogger::buildFilePath(time_t utc)
{
    time_t adjusted = utc;
    if (localMinuteOfDay(utc) < ROTATION_MINUTE) {
        adjusted = utc - 86400;  // veille
    }

    struct tm local;
    localtime_r(&adjusted, &local);

    snprintf(logFilePath, sizeof(logFilePath), "/log_%04d-%02d-%02d.csv",
             local.tm_year + 1900,
             local.tm_mon + 1,
             local.tm_mday);
}

// -----------------------------------------------------------------------------
// Gestion fichier — ouverture / fermeture
// -----------------------------------------------------------------------------
void DataLogger::ensureFileOpen()
{
    if (logFileOpen) return;

    TimeVClock t = VirtualClock::read();
    if (!t.VClock_available) return;  // Pas d'horloge = pas de nom de fichier

    buildFilePath(t.timestamp);

    logFile = LittleFS.open(logFilePath, FILE_APPEND);
    if (!logFile) {
        Console::error(TAG, "Impossible d'ouvrir " + String(logFilePath));
        return;
    }

    logFileOpen = true;

    // Initialiser lastRotationDate au premier open
    if (lastRotationDate == -1) {
        int today = localDate(t.timestamp);
        if (localMinuteOfDay(t.timestamp) >= ROTATION_MINUTE) {
            lastRotationDate = today;
        } else {
            // Avant l'heure de rotation : la rotation du jour n'a pas encore
            // eu lieu. On positionne sur la veille pour que la rotation
            // se déclenche à l'heure prévue.
            time_t yesterday = t.timestamp - 86400;
            lastRotationDate = localDate(yesterday);
        }
    }
}

void DataLogger::closeFile()
{
    if (logFileOpen) {
        logFile.close();
        logFileOpen = false;
    }
}

// -----------------------------------------------------------------------------
// Rotation quotidienne
//
// Condition : heure locale ≥ heure de rotation ET date du jour ≠ dernière
// rotation. Ferme le fichier courant, ouvre un nouveau fichier avec la date
// du jour. Supprime les fichiers au-delà de la rétention configurée.
// -----------------------------------------------------------------------------
void DataLogger::checkRotation()
{
    TimeVClock t = VirtualClock::read();
    if (!t.VClock_available) return;
    if (lastRotationDate == -1) return;  // Pas encore initialisé

    int today     = localDate(t.timestamp);
    int minuteNow = localMinuteOfDay(t.timestamp);

    if (minuteNow >= ROTATION_MINUTE && today != lastRotationDate) {
        // Écrire le buffer actif s'il contient des données
        if (activeLen > 0) {
            swapAndFlush();
        }

        closeFile();
        Console::info(TAG, "Rotation — fichier clôturé : " + String(logFilePath));
        lastRotationDate = today;

        // Le nouveau fichier sera ouvert au prochain writeFlushBuffer/ensureFileOpen

        cleanupOldFiles();
    }
}

// -----------------------------------------------------------------------------
// Nettoyage — suppression des fichiers au-delà de la rétention
// -----------------------------------------------------------------------------
void DataLogger::cleanupOldFiles()
{
    TimeVClock t = VirtualClock::read();
    if (!t.VClock_available) return;

    time_t cutoff = t.timestamp - ((time_t)DATALOGGER_RETENTION_DAYS * 86400UL);

    File root = LittleFS.open("/");
    if (!root) return;

    File f = root.openNextFile();
    while (f) {
        String name = String(f.name());
        f.close();

        // Filtrer les fichiers log : /log_YYYY-MM-DD.csv ou log_YYYY-MM-DD.csv
        // (le préfixe '/' dépend de la version du core ESP32)
        int y, m, d;
        const char* p = name.c_str();
        if (p[0] == '/') p++;

        if (sscanf(p, "log_%d-%d-%d.csv", &y, &m, &d) == 3) {
            struct tm tmFile = {};
            tmFile.tm_year = y - 1900;
            tmFile.tm_mon  = m - 1;
            tmFile.tm_mday = d;
            tmFile.tm_hour = 12;  // milieu de journée pour éviter les effets DST
            time_t fileDate = mktime(&tmFile);

            if (fileDate > 0 && fileDate < cutoff) {
                String fullPath = name.startsWith("/") ? name : ("/" + name);
                if (LittleFS.remove(fullPath)) {
                    Console::info(TAG, "Ancien log supprimé : " + fullPath);
                }
            }
        }

        f = root.openNextFile();
    }
    root.close();
}

// -----------------------------------------------------------------------------
// PENDING — FIFO circulaire avec perte FIFO (anciens perdus si plein)
// -----------------------------------------------------------------------------
void DataLogger::addPending(const DataRecord& r)
{
    if (pendingCount == PENDING_SIZE) {
        pendingHead = (pendingHead + 1) % PENDING_SIZE;
        pendingCount--;
    }

    size_t index = (pendingHead + pendingCount) % PENDING_SIZE;
    pending[index] = r;
    pendingCount++;
}

// -----------------------------------------------------------------------------
// Sérialisation d'un DataRecord en CSV dans le buffer actif
//
// Format CSV : timestamp,VClock_available,VClock_reliable,type,id,valueType,value
// Retourne false si le buffer n'a pas assez de place.
// -----------------------------------------------------------------------------
bool DataLogger::serializeToActive(const DataRecord& r)
{
    char line[256];  // Marge pour les records texte (Boot, Error, SmsEvent)
    int len;

    if (std::holds_alternative<float>(r.value)) {
        float val = std::get<float>(r.value);
        len = snprintf(line, sizeof(line), "%lu,%d,%d,%d,%d,0,%.3f\n",
                       (unsigned long)r.timestamp,
                       (int)r.VClock_available,
                       (int)r.VClock_reliable,
                       (int)r.type,
                       (int)r.id,
                       val);
    } else {
        const String& txt = std::get<String>(r.value);
        String escaped = escapeCSV(txt);
        len = snprintf(line, sizeof(line), "%lu,%d,%d,%d,%d,1,%s\n",
                       (unsigned long)r.timestamp,
                       (int)r.VClock_available,
                       (int)r.VClock_reliable,
                       (int)r.type,
                       (int)r.id,
                       escaped.c_str());
    }

    if (len <= 0 || len >= (int)sizeof(line)) return false;  // Erreur format
    if (activeLen + (size_t)len > CHUNK_BUFFER_SIZE) return false;  // Pas assez de place

    memcpy(activeBuffer + activeLen, line, len);
    activeLen   += (size_t)len;
    activeCount += 1;
    return true;
}

// -----------------------------------------------------------------------------
// Swap des buffers + écriture flash
// -----------------------------------------------------------------------------
void DataLogger::swapAndFlush()
{
    // Si le précédent flush n'a pas abouti, tenter de l'écrire maintenant
    if (flushLen > 0) {
        writeFlushBuffer();
        if (flushLen > 0) {
            Console::error(TAG, "Échec double flush — données perdues ("
                           + String(flushLen) + " octets)");
            flushLen = 0;
        }
    }

    // Swap des pointeurs
    char* tmp    = activeBuffer;
    activeBuffer = flushBuffer;
    flushBuffer  = tmp;

    flushLen    = activeLen;
    activeLen   = 0;
    activeCount = 0;

    // Écriture immédiate
    writeFlushBuffer();
}

// -----------------------------------------------------------------------------
// Écriture du buffer de flush dans le fichier courant
// Un seul write + un seul flush + yield — coût typique 30-80 ms.
// -----------------------------------------------------------------------------
void DataLogger::writeFlushBuffer()
{
    if (flushLen == 0) return;

    ensureFileOpen();
    if (!logFileOpen) return;  // Pas d'horloge ou erreur FS

    size_t written = logFile.write((const uint8_t*)flushBuffer, flushLen);
    if (written != flushLen) {
        Console::error(TAG, "Écriture partielle : " + String(written)
                       + "/" + String(flushLen) + " octets");
        // On considère tout de même le flush fait pour éviter une boucle
    }

    logFile.flush();
    flushLen = 0;
    yield();
    Console::info(TAG, "Chunk écrit : " + String(written) + " oct → " + String(logFilePath));
}

// -----------------------------------------------------------------------------
// HANDLE — cœur du logger, appelé toutes les 3 secondes
// -----------------------------------------------------------------------------
void DataLogger::handle()
{
    // ── 1. Drain de la logQueue DataBus → PENDING ──────────────────────────
    BusItem item;
    while (DataBus::tryPopLog(item)) {
        DataRecord rec;
        rec.timestamp        = item.timestamp;
        rec.VClock_available = item.VClock_available;
        rec.VClock_reliable  = item.VClock_reliable;
        rec.type             = item.type;
        rec.id               = item.id;
        if (item.valueKind == 0) {
            rec.value = item.valueFloat;
        } else {
            rec.value = String(item.valueText);
        }
        addPending(rec);
    }

    // ── 2. Réparation des timestamps millis() → UTC ────────────────────────
    TimeVClock t = VirtualClock::read();
    if (t.VClock_available) {
        for (size_t i = 0; i < pendingCount; ++i) {
            size_t idx = (pendingHead + i) % PENDING_SIZE;
            if (!pending[idx].VClock_available) {
                int32_t deltaMs = static_cast<int32_t>(millis() - pending[idx].timestamp);
                time_t repaired = t.timestamp - static_cast<time_t>(deltaMs / 1000L);

                if (repaired > 0) {
                    pending[idx].timestamp       = static_cast<uint32_t>(repaired);
                    pending[idx].VClock_available = true;
                    pending[idx].VClock_reliable  = t.VClock_reliable;
                }
            }
        }
    }

    // ── 3. Transfert d'UN SEUL record réparé → buffer actif ────────────────
    if (pendingCount > 0) {
        size_t idx = pendingHead;
        if (pending[idx].VClock_available) {
            if (!serializeToActive(pending[idx])) {
                // Buffer plein — swap + flush, puis réessai
                swapAndFlush();
                if (!serializeToActive(pending[idx])) {
                    Console::error(TAG, "Record trop grand pour le buffer (id="
                                   + String((uint8_t)pending[idx].id) + ")");
                }
            }
            // Retirer de PENDING
            pendingHead = (pendingHead + 1) % PENDING_SIZE;
            pendingCount--;
        }
    }

    // ── 4. Seuil de records atteint → swap + flush ─────────────────────────
    if (activeCount >= CHUNK_RECORD_LIMIT) {
        swapAndFlush();
    }

    // ── 5. Sécurité : flush en attente non écrit (échec précédent) ─────────
    if (flushLen > 0) {
        writeFlushBuffer();
    }

    // ── 6. Vérification rotation quotidienne ───────────────────────────────
    checkRotation();
}

// -----------------------------------------------------------------------------
// CLEAR HISTORY — supprime tous les fichiers log et réinitialise les buffers
// -----------------------------------------------------------------------------
void DataLogger::clearHistory()
{
    Console::info(TAG, "Suppression de l'historique...");

    closeFile();

    // Supprimer tous les fichiers log_*.csv
    File root = LittleFS.open("/");
    if (root) {
        // Collecter les noms d'abord (éviter modification pendant itération)
        String toDelete[64];
        size_t count = 0;

        File f = root.openNextFile();
        while (f && count < 64) {
            String name = String(f.name());
            f.close();

            const char* p = name.c_str();
            if (p[0] == '/') p++;
            if (strncmp(p, "log_", 4) == 0 && strstr(p, ".csv") != nullptr) {
                toDelete[count++] = name.startsWith("/") ? name : ("/" + name);
            }

            f = root.openNextFile();
        }
        root.close();

        for (size_t i = 0; i < count; i++) {
            if (LittleFS.remove(toDelete[i])) {
                Console::info(TAG, "Supprimé : " + toDelete[i]);
            }
        }
    }

    pendingHead  = 0;
    pendingCount = 0;
    activeLen    = 0;
    activeCount  = 0;
    flushLen     = 0;

    Console::info(TAG, "Buffers réinitialisés. Historique vidé.");
}

// -----------------------------------------------------------------------------
// STATISTIQUES D'UTILISATION DE LA FLASH
// -----------------------------------------------------------------------------
FlashUsageStats DataLogger::getFlashUsageStats()
{
    FlashUsageStats stats;
    stats.mounted                = false;
    stats.flashTotalBytes        = ESP.getFlashChipSize();
    stats.appUsedBytes           = ESP.getSketchSize();
    stats.appPartitionBytes      = 0;
    stats.littlefsPartitionBytes = 0;
    stats.littlefsUsedBytes      = 0;
    stats.datalogFileBytes       = 0;
    stats.ramPeakPercent         = 0;
    stats.ramCurrentPercent      = 0;

    const esp_partition_t* appPart = esp_ota_get_running_partition();
    if (appPart != NULL) {
        stats.appPartitionBytes = appPart->size;
    }

    size_t littlefsTotal = LittleFS.totalBytes();
    if (littlefsTotal == 0) {
        return stats;
    }

    stats.mounted                = true;
    stats.littlefsPartitionBytes = littlefsTotal;
    stats.littlefsUsedBytes      = LittleFS.usedBytes();

    // Cumul de tous les fichiers log_*.csv
    size_t totalLogBytes = 0;
    File root = LittleFS.open("/");
    if (root) {
        File f = root.openNextFile();
        while (f) {
            String name = String(f.name());
            const char* p = name.c_str();
            if (p[0] == '/') p++;
            if (strncmp(p, "log_", 4) == 0 && strstr(p, ".csv") != nullptr) {
                totalLogBytes += f.size();
            }
            f.close();
            f = root.openNextFile();
        }
        root.close();
    }
    stats.datalogFileBytes = totalLogBytes;

    size_t heapSize    = ESP.getHeapSize();
    size_t minFreeHeap = ESP.getMinFreeHeap();
    if (heapSize > 0) {
        stats.ramPeakPercent = (uint8_t)(((heapSize - minFreeHeap) * 100ULL) / heapSize);
    }

    size_t freeHeap = ESP.getFreeHeap();
    if (heapSize > 0) {
        stats.ramCurrentPercent = (uint8_t)(((heapSize - freeHeap) * 100ULL) / heapSize);
    }

    return stats;
}

// -----------------------------------------------------------------------------
// AFFICHAGE CONSOLE
// -----------------------------------------------------------------------------
void DataLogger::logFlashUsage()
{
    FlashUsageStats s = getFlashUsageStats();

    if (!s.mounted) {
        Console::error(TAG, "⚠️ LittleFS non disponible — état de la flash indisponible");
        return;
    }

    constexpr float MB = 1024.0f * 1024.0f;
    char buf[160];

    int appPct = (s.appPartitionBytes > 0)
        ? (int)((s.appUsedBytes * 100ULL + s.appPartitionBytes / 2)
                / s.appPartitionBytes)
        : 0;
    int spPct  = (s.littlefsPartitionBytes > 0)
        ? (int)((s.littlefsUsedBytes * 100ULL + s.littlefsPartitionBytes / 2)
                / s.littlefsPartitionBytes)
        : 0;

    snprintf(buf, sizeof(buf), "═══ État de la flash (%.2f MB) ═══",
             s.flashTotalBytes / MB);
    Console::info(TAG, String(buf));

    snprintf(buf, sizeof(buf),
             "  Programme : %.2f MB / %.2f MB partition  (%d%% partition)",
             s.appUsedBytes / MB, s.appPartitionBytes / MB, appPct);
    Console::info(TAG, String(buf));

    snprintf(buf, sizeof(buf),
             "  Données   : %.2f MB / %.2f MB partition (%d%% partition)",
             s.littlefsUsedBytes / MB, s.littlefsPartitionBytes / MB, spPct);
    Console::info(TAG, String(buf));

    Console::info(TAG, "═══════════════════════════════════");
}