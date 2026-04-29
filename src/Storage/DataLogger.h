// Storage/DataLogger.h
// Logger LittleFS — persistance CSV avec double buffer et rotation quotidienne.
//
// Reçoit ses données via DataBus::logQueue (FreeRTOS).
// Flux : drain logQueue → PENDING → réparation timestamps → sérialisation CSV
//        → double buffer char[512] → write + flush + yield → rotation quotidienne.
// Types et enums centralisés dans Config/MetaDataModel.h.
#pragma once

#include <Arduino.h>
#include <LittleFS.h>
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
    size_t datalogFileBytes;         // Taille totale de tous les fichiers log_*.csv.
                                     // Avec la rotation quotidienne, plusieurs fichiers
                                     // coexistent. Ce champ donne le cumul.
    uint8_t ramPeakPercent;          // Pic d'utilisation RAM (heap) depuis le boot, en %.
    uint8_t ramCurrentPercent;       // Utilisation RAM instantanée au moment de l'appel, en %.
};

// ═════════════════════════════════════════════════════════════════════════════
// DataLogger — logger LittleFS avec double buffer et rotation quotidienne
//
// Principe :
//   - PENDING accumule les records bruts et répare les timestamps (boot).
//   - handle() déplace UN SEUL record réparé par appel vers le buffer actif.
//   - Le buffer actif est un char[512] où les records sont sérialisés en CSV.
//   - À 14 records (ou si le buffer est plein), swap + écriture flash.
//   - Un seul file.write() + un seul file.flush() + yield() par chunk.
//   - Rotation quotidienne : fermeture du fichier courant, ouverture d'un
//     nouveau fichier nommé par date (log_YYYY-MM-DD.csv).
//   - Fichier ouvert en permanence en mode append.
// ═════════════════════════════════════════════════════════════════════════════

class DataLogger {
public:
    static void init();

    // Drain logQueue (DataBus) → PENDING → réparation UTC → sérialisation
    // CSV → double buffer → flush LittleFS. UN seul record par appel.
    static void handle();

    static void clearHistory();

    // Statistiques d'utilisation de la flash (programme + LittleFS).
    static FlashUsageStats getFlashUsageStats();

    // Affiche l'état de la flash sur la console série.
    static void logFlashUsage();

private:
    // ───────────── PENDING — FIFO circulaire (réparation timestamps) ─────────
    // Taille calibrée pour le boot : ~5 min à période 3 s ≈ 100 records max.
    // En régime permanent, PENDING est quasi vide (traversée immédiate).
    static constexpr size_t PENDING_SIZE = 100;

    static DataRecord pending[PENDING_SIZE];
    static size_t     pendingHead;
    static size_t     pendingCount;

    static void addPending(const DataRecord& r);

    // ───────────── Double buffer CSV (ping-pong) ─────────────────────────────
    // Deux buffers de 512 octets. L'actif reçoit les lignes CSV sérialisées.
    // À 14 records (~420 octets), swap : l'actif devient le buffer à flusher,
    // l'autre redevient actif immédiatement. Écriture flash sur le buffer
    // inactif uniquement.
    static constexpr size_t CHUNK_BUFFER_SIZE  = 512;
    static constexpr size_t CHUNK_RECORD_LIMIT = 14;

    static char   bufferA[CHUNK_BUFFER_SIZE];
    static char   bufferB[CHUNK_BUFFER_SIZE];
    static char*  activeBuffer;       // Pointe vers bufferA ou bufferB
    static char*  flushBuffer;        // Pointe vers l'autre
    static size_t activeLen;          // Octets écrits dans le buffer actif
    static size_t activeCount;        // Nombre de records dans le buffer actif
    static size_t flushLen;           // Octets à écrire (0 = rien à flusher)

    // Sérialise un DataRecord en CSV à la fin du buffer actif.
    // Retourne false si le buffer n'a pas assez de place.
    static bool serializeToActive(const DataRecord& r);

    // Swap les buffers et écrit le buffer inactif sur flash.
    static void swapAndFlush();

    // Écrit le contenu de flushBuffer dans le fichier courant.
    static void writeFlushBuffer();

    // ───────────── Gestion fichier et rotation ───────────────────────────────
    static File logFile;              // Fichier courant, ouvert en permanence
    static bool logFileOpen;
    static char logFilePath[32];      // Ex. "/log_2026-04-28.csv"
    static int  lastRotationDate;     // YYYYMMDD, -1 = pas encore initialisé

    // Ouvre le fichier courant si nécessaire (calcul du nom par date).
    static void ensureFileOpen();

    // Ferme le fichier courant.
    static void closeFile();

    // Vérifie si l'heure de rotation est atteinte et effectue la rotation.
    static void checkRotation();

    // Construit le chemin du fichier log pour un instant UTC donné.
    // Tient compte de l'heure de rotation (avant → date veille, après → date du jour).
    static void buildFilePath(time_t utc);

    // Supprime les fichiers log plus anciens que DATALOGGER_RETENTION_DAYS.
    static void cleanupOldFiles();

    // Utilitaires date/heure locale
    static int localDate(time_t utc);        // Retourne YYYYMMDD
    static int localMinuteOfDay(time_t utc);  // Retourne heure*60 + minute
};