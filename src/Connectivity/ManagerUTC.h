// Connectivity/ManagerUTC.h
#pragma once

#include <Arduino.h>
#include <time.h>

/*
 * ManagerUTC — Maître du temps + Agent NTP
 *
 * Rôle 1 — Fournisseur de temps :
 *  - readUTC() : point d'accès UNIQUE au temps pour tout le système
 *  - Cascade : RTC → VirtualClock → millis()
 *  - Retourne TOUJOURS un temps exploitable
 *
 * Rôle 2 — Agent NTP :
 *  - Au boot : tentatives rapides (30s) jusqu'à 10 essais
 *  - En routine : un essai toutes les 50 min (VClock pas synced)
 *    ou tous les 25 tours (~20h50, VClock synced)
 *  - À chaque synchro NTP → écriture RTCManager + sync VirtualClock
 */

// ─────────────────────────────────────────────
// Struct de retour de readUTC()
//
// Cas 1 : RTC OK        → {UTC,       true,  true }
// Cas 2 : VClock synced → {UTC approx, true,  false}
// Cas 3 : Rien          → {millis(),   false, false}
//
// timestamp contient TOUJOURS un temps exploitable.
// UTC_available dit si c'est de l'UTC.
// UTC_reliable dit si l'UTC est précis (RTC) ou approximatif (VClock).
// UTC_reliable n'a de sens que si UTC_available == true.
// ─────────────────────────────────────────────

struct TimeUTC {
    time_t  timestamp;      // UTC si UTC_available, millis() sinon
    bool    UTC_available;   // true = timestamp est un temps UTC
    bool    UTC_reliable;    // true = RTC (précis), false = VClock (dérive)
};

class ManagerUTC {
public:
    static void init();
    static void handle();   // à appeler régulièrement (loop)

    // Point d'accès unique au temps — ne retourne jamais 0
    static TimeUTC readUTC();

private:
    static bool trySync();

    static bool     _everSynced;
    static uint32_t _networkUpSinceMs;
    static uint32_t _lastAttemptMs;
    static uint32_t _lastTourMs;
    static uint8_t  _bootAttempts;
    static uint8_t  _tourCount;
};