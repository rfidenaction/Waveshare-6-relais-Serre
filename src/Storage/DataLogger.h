// Storage/DataLogger.h
// Logger pur — persistance SPIFFS et statistiques flash/RAM.
//
// Depuis le refactor DataBus, ce module ne contient plus :
//   - Les définitions de types/enums/META (migrés dans Config/MetaDataModel.h)
//   - Les buffers LogBufferIn/LogBufferOut (remplacés par les queues DataBus)
//   - Le ring buffer LIVE (supprimé)
//   - Les fonctions submit/push/traceCommand (remplacées par DataBus::publish)
//   - parseCommand/ParsedCommand/CommandParseResult (migrés dans DataBus)
//   - lastDataForWeb (migré dans WebServer)
//   - tryPopForPublish (remplacé par DataBus::tryPopMqtt)
//   - getLastUtcRecord (supprimée, inutilisée)
//
// DataLogger reçoit désormais ses données via DataBus::logQueue (FreeRTOS)
// et ne fait plus que : drain → PENDING → réparation timestamps → flush SPIFFS.
#pragma once

#include <Arduino.h>
#include "Config/MetaDataModel.h"

// ═════════════════════════════════════════════════════════════════════════════
// Statistiques d'utilisation de la flash
//
// Source d'information unique pour les trois canaux de présentation :
//   - Console série (logFlashUsage() au boot)
//   - Page web /logs (PageLogs::getHtml)
//   - MQTT et futur serveur HTTP (publication à venir, hors périmètre actuel) :
//     les champs en octets bruts permettent au consommateur distant de calculer
//     ses propres seuils d'alerte sans dépendance au formatage firmware.
//
// Tous les champs sont en octets, sauf indication contraire. Le formatage en
// MB et en pourcentages est fait au moment de l'affichage par chaque canal.
// ═════════════════════════════════════════════════════════════════════════════

struct FlashUsageStats {
    bool   mounted;                  // LittleFS opérationnelle (false = LittleFS non montée)
    size_t flashTotalBytes;          // Taille flash physique (puce, ex. 16 MB)
    size_t appPartitionBytes;        // Taille de la partition app
    size_t appUsedBytes;             // Programme utilisé dans la partition app
    size_t littlefsPartitionBytes;   // Taille de la partition LittleFS
    size_t littlefsUsedBytes;        // Occupation totale LittleFS (datalog + tout autre fichier)
    size_t datalogFileBytes;     // Taille du fichier /datalog.csv seul.
                                 // Champ dédié à la barre de progression du
                                 // téléchargement (PageLogs JS), conservé pour
                                 // ne rien casser dans le flux d'export existant.
    uint8_t ramPeakPercent;      // Pic d'utilisation RAM (heap) depuis le boot, en %.
                                 // Calcul : (heapSize - minFreeHeap) * 100 / heapSize.
                                 // Utile pour savoir si on peut augmenter les buffers
                                 // ou si on est proche de la saturation.
    uint8_t ramCurrentPercent;   // Utilisation RAM instantanée au moment de l'appel, en %.
                                 // Calcul : (heapSize - freeHeap) * 100 / heapSize.
};

// ═════════════════════════════════════════════════════════════════════════════
// DataLogger — logger SPIFFS pur
// ═════════════════════════════════════════════════════════════════════════════

class DataLogger {
public:
    static void init();

    // Drain logQueue (DataBus) → PENDING → réparation UTC → flush SPIFFS.
    static void handle();

    static void clearHistory();

    // Statistiques d'utilisation de la flash (programme + LittleFS).
    static FlashUsageStats getFlashUsageStats();

    // Affiche l'état de la flash sur la console série.
    static void logFlashUsage();

private:
    // ───────────── PENDING — FIFO circulaire ─────────────
    static constexpr size_t PENDING_SIZE = 2000;
    static constexpr size_t FLUSH_SIZE   = 50;

    static DataRecord pending[PENDING_SIZE];
    static size_t     pendingHead;
    static size_t     pendingCount;

    static void addPending(const DataRecord& r);
    static void tryFlush();
    static void flushToFlash(size_t count);
};
