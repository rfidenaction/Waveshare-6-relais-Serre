// Storage/DataLogger.cpp
// Logger SPIFFS pur — voir DataLogger.h
//
// Depuis le refactor DataBus :
//   - handle() draine DataBus::logQueue via tryPopLog()
//   - Les records arrivent déjà horodatés et typés par DataBus::publish()
//   - Plus de LogBufferIn/LogBufferOut, plus de LIVE, plus de lastDataForWeb
//   - Réparation UTC et flush SPIFFS inchangés
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
// Buffers
// -----------------------------------------------------------------------------
DataRecord DataLogger::pending[PENDING_SIZE];
size_t DataLogger::pendingHead  = 0;
size_t DataLogger::pendingCount = 0;

static unsigned long lastFlushMs = 0;

// -----------------------------------------------------------------------------
// Initialisation
// -----------------------------------------------------------------------------
void DataLogger::init()
{
    logFlashUsage();

    lastFlushMs = millis();

    pendingHead  = 0;
    pendingCount = 0;
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

    size_t index =
        (pendingHead + pendingCount) % PENDING_SIZE;

    pending[index] = r;
    pendingCount++;
}

// -----------------------------------------------------------------------------
// HANDLE — drain logQueue + réparation + flush
// -----------------------------------------------------------------------------
void DataLogger::handle()
{
    // Drain de la logQueue DataBus : chaque BusItem est converti en DataRecord
    // puis injecté dans PENDING pour réparation et persistence.
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

    // Réparation des timestamps millis() → UTC quand VClock bascule available
    TimeVClock t = VirtualClock::read();
    if (t.VClock_available) {
        for (size_t i = 0; i < pendingCount; ++i) {
            size_t idx = (pendingHead + i) % PENDING_SIZE;
            if (!pending[idx].VClock_available) {
                int32_t deltaMs = static_cast<int32_t>(millis() - pending[idx].timestamp);
                time_t repaired = t.timestamp - static_cast<time_t>(deltaMs / 1000L);

                if (repaired > 0) {
                    pending[idx].timestamp        = static_cast<uint32_t>(repaired);
                    pending[idx].VClock_available = true;
                    pending[idx].VClock_reliable  = t.VClock_reliable;
                }
            }
        }
    }

    bool flushByCount = (pendingCount >= FLUSH_SIZE);
    bool flushByTime  = (pendingCount > 0
                         && (millis() - lastFlushMs) >= FLUSH_HOURLY_MIN_INTERVAL_MS);

    if (flushByCount || flushByTime) {
        tryFlush();
    }
}

// -----------------------------------------------------------------------------
// TRY FLUSH — flushe les records VClock_available contigus depuis la tête
// -----------------------------------------------------------------------------
void DataLogger::tryFlush()
{
    size_t flushable = 0;
    for (size_t i = 0; i < pendingCount; ++i) {
        size_t idx = (pendingHead + i) % PENDING_SIZE;
        if (pending[idx].VClock_available) {
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
// Format CSV : timestamp,VClock_available,VClock_reliable,type,id,valueType,value
// valueType = 0 pour float, 1 pour String
// -----------------------------------------------------------------------------
void DataLogger::flushToFlash(size_t count)
{
    File f = LittleFS.open("/datalog.csv", FILE_APPEND);
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
                     (int)r.VClock_available,
                     (int)r.VClock_reliable,
                     (int)r.type,
                     (int)r.id,
                     val);
        } else {
            String txt = std::get<String>(r.value);
            String escaped = escapeCSV(txt);
            f.printf("%lu,%d,%d,%d,%d,1,%s\n",
                     r.timestamp,
                     (int)r.VClock_available,
                     (int)r.VClock_reliable,
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
// CLEAR HISTORY
// -----------------------------------------------------------------------------
void DataLogger::clearHistory()
{
    Console::info(TAG, "Suppression de l'historique...");

    if (LittleFS.remove("/datalog.csv")) {
        Console::info(TAG, "Fichier /datalog.csv supprimé avec succès");
    } else {
        Console::warn(TAG, "Impossible de supprimer /datalog.csv (peut-être inexistant)");
    }

    pendingHead = 0;
    pendingCount = 0;

    Console::info(TAG, "Buffers réinitialisés. Historique vidé.");
}

// -----------------------------------------------------------------------------
// STATISTIQUES D'UTILISATION DE LA FLASH
// -----------------------------------------------------------------------------
FlashUsageStats DataLogger::getFlashUsageStats()
{
    FlashUsageStats stats;
    stats.mounted              = false;
    stats.flashTotalBytes      = ESP.getFlashChipSize();
    stats.appUsedBytes         = ESP.getSketchSize();
    stats.appPartitionBytes    = 0;
    stats.littlefsPartitionBytes = 0;
    stats.littlefsUsedBytes      = 0;
    stats.datalogFileBytes     = 0;
    stats.ramPeakPercent       = 0;
    stats.ramCurrentPercent    = 0;

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

    File f = LittleFS.open("/datalog.csv", FILE_READ);
    if (f) {
        stats.datalogFileBytes = f.size();
        f.close();
    }

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
