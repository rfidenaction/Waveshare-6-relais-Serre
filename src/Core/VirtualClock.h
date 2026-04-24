// Core/VirtualClock.h
// Horloge virtuelle machine — source unique du temps runtime
//
// VirtualClock est le SEUL point d'accès au temps pour le reste du firmware.
// Les autres modules de temps (RTCManager, NTPManager) ne servent qu'à
// l'alimenter via sync() ; ils ne sont jamais interrogés directement par
// les consommateurs de temps.
//
// Comportement résumé :
//  - init() : _available=false, _reliable=false. Aucune ancre posée.
//  - Avant T+4min : NTPManager peut appeler sync() → bascule immédiate.
//  - À T+4min (handle()) : dernier recours RTC. OK → _reliable=true.
//                          KO → ancre 12h30 arbitraire + _reliable=false.
//  - Régime permanent : resync RTC toutes les 3h (cadence absolue),
//                       bascule _reliable=false si 24h sans sync.
//
// read() retourne toujours une struct exploitable :
//  - VClock_available=false → timestamp contient (time_t)millis() (T < 4min)
//  - VClock_available=true  → timestamp contient un UTC (secondes Unix)
//  - VClock_reliable n'a de sens que si VClock_available=true.
//    true  : source synchronisée < 24h (RTC valide ou NTP)
//    false : ancre 12h30 arbitraire ou dérive > 24h sans sync
//    Non consulté en runtime : uniquement lu à l'analyse des logs.
#pragma once

#include <Arduino.h>
#include <time.h>

// ─────────────────────────────────────────────
// Struct de retour de VirtualClock::read()
// ─────────────────────────────────────────────
struct TimeVClock {
    time_t timestamp;         // UTC si VClock_available, (time_t)millis() sinon
    bool   VClock_available;  // true = timestamp est un temps UTC
    bool   VClock_reliable;   // true = source synchronisée < 24h
};

class VirtualClock {
public:
    // Initialisation : pose les booléens à false, pas d'ancre.
    static void init();

    // Tâche périodique (TaskManager, VCLOCK_HANDLE_PERIOD_MS).
    static void handle();

    // Recalage depuis source fiable (NTP, ou RTC via handle()).
    // Pose _available=true, _reliable=true, met _lastSyncMillis à jour.
    static void sync(time_t utc);

    // Lecture unique du temps. TOUJOURS une struct exploitable.
    static TimeVClock read();

private:
    static time_t   _anchorUtc;         // UTC au dernier recalage d'ancre
    static uint32_t _anchorMillis;      // millis() au dernier recalage d'ancre
    static uint32_t _lastSyncMillis;    // millis() à la dernière vraie sync (0 = jamais)
    static uint32_t _lastRtcResyncMs;   // millis() à la dernière resync RTC tentée
    static bool     _available;
    static bool     _reliable;
};
