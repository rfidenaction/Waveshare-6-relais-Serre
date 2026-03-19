// Core/VirtualClock.h
// Horloge virtuelle machine — backup pour les actions de la serre
//
// Principe :
//  - Démarre à midi (heure France) au boot
//  - Recalée par RTC (toutes les 24h) et par NTP (à chaque synchro)
//  - Entre deux recalages, dérive sur millis()
//
// Usage actuel :
//  - Timestamps du buffer Live (DataLogger)
//
// Usage futur :
//  - Logique métier arrosage quand le RTC est invalide
//
// ATTENTION : cette heure peut être totalement fausse si jamais
// recalée. Ne JAMAIS l'utiliser pour l'horodatage SPIFFS / MQTT.
#pragma once

#include <Arduino.h>
#include <time.h>

class VirtualClock {
public:
    // Initialisation : ancre à midi heure France
    static void init();

    // Appel périodique : resync depuis RTC toutes les 24h si fiable
    static void handle();

    // Heure courante (time_t, éventuellement approximative)
    static time_t nowVirtual();

    // Recalage depuis une source fiable (RTC ou NTP)
    static void sync(time_t utc);

    // L'horloge a-t-elle été recalée au moins une fois ?
    static bool isSynced();

private:
    // Point d'ancrage
    static time_t   _anchorUtc;
    static uint32_t _anchorMillis;

    // État
    static bool     _synced;
    static uint32_t _lastSyncMs;

    // Période de resync depuis RTC
    static constexpr uint32_t RESYNC_PERIOD_MS = 24UL * 60UL * 60UL * 1000UL; // 24h
};