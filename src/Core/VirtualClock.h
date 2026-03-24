// Core/VirtualClock.h
// Horloge virtuelle machine — backup pour les actions de la serre
//
// Principe :
//  - Démarre à midi (heure France) au boot
//  - Recalée par RTC au boot et par NTP (via ManagerUTC)
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

    // Heure courante (time_t, éventuellement approximative)
    static time_t nowVirtual();

    // Recalage depuis une source fiable (RTC ou NTP)
    static void sync(time_t utc);

    // L'horloge a-t-elle été recalée au moins une fois ?
    static bool isVClockSynced();

private:
    // Point d'ancrage
    static time_t   _anchorUtc;
    static uint32_t _anchorMillis;

    // État
    static bool     _VClockSynced;
};