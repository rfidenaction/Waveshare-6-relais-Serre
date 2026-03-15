// Connectivity/ManagerUTC.h
#pragma once

#include <Arduino.h>
#include <time.h>

/*
 * ManagerUTC
 *
 * Source unique de vérité du temps UTC.
 *
 * - Synchronisation NTP via Wi-Fi
 * - UTC invalide par défaut
 * - UTC invalide après 25h sans NTP
 * - Aucune approximation temporelle
 * - Aucune persistance après reboot
 *
 * Le système peut fonctionner entièrement sans UTC.
 */

class ManagerUTC {
public:
    // Cycle de vie
    static void init();
    static void handle();   // à appeler régulièrement (loop)

    // API
    static bool   isUtcValid();
    static time_t nowUtc();

    // Conversion relatif → UTC (DataLogger)
    static time_t convertFromRelative(uint32_t t_rel_ms);

private:
    // Synchronisation
    static bool trySync();

    // État UTC
    static bool     utcValid;
    static bool     everSynced;

    // Timers
    static uint32_t networkUpSinceMs;
    static uint32_t lastAttemptMs;
    static uint32_t lastSyncMs;

    // Politique
    static uint8_t  bootAttempts;

    // Référence temporelle
    static uint32_t syncRelMs;
    static time_t   syncUtc;
};
