// Core/VirtualClock.cpp
// Horloge virtuelle machine — backup pour les actions de la serre
#include "Core/VirtualClock.h"
#include "Config/Config.h"
#include "Utils/Console.h"

static const char* TAG = "VClock";

// État interne
time_t   VirtualClock::_anchorUtc    = 0;
uint32_t VirtualClock::_anchorMillis = 0;
bool     VirtualClock::_VClockSynced = false;

// -----------------------------------------------------------------------------
// Helper : formater un time_t en heure locale France (via SYSTEM_TIMEZONE)
// -----------------------------------------------------------------------------
static String formatLocalTime(time_t utc)
{
    struct tm tmLocal;
    localtime_r(&utc, &tmLocal);

    char buf[32];
    snprintf(buf, sizeof(buf), "%02d/%02d/%04d %02d:%02d:%02d",
             tmLocal.tm_mday,
             tmLocal.tm_mon + 1,
             tmLocal.tm_year + 1900,
             tmLocal.tm_hour,
             tmLocal.tm_min,
             tmLocal.tm_sec);
    return String(buf);
}

// -----------------------------------------------------------------------------
// Initialisation : ancre à 12h30 heure France
// On calcule 12h30 local via SYSTEM_TIMEZONE pour respecter CET/CEST.
// Date arbitraire 2025-01-01 (hiver → CET → 12h30 local = 11:30 UTC)
// La date n'a pas d'importance pour le rythme d'arrosage,
// seule l'heure du jour compte.
//
// 12h30 est choisi pour être cohérent avec SafeReboot :
// le reboot mensuel a lieu à 12h25, VirtualClock démarre à 12h30
// → si le RTC est mort et pas de NTP, l'erreur est de ~5 minutes.
// -----------------------------------------------------------------------------
void VirtualClock::init()
{
    // 2025-01-01 11:30:00 UTC = 12h30 CET
    _anchorUtc      = 1735731000UL;
    _anchorMillis   = millis();
    _VClockSynced   = false;

    Console::info(TAG, "Initialisée à " + formatLocalTime(_anchorUtc) + " (heure locale arbitraire)");
}

// -----------------------------------------------------------------------------
// Heure courante
// Interpole depuis le dernier point d'ancrage via millis()
// -----------------------------------------------------------------------------
time_t VirtualClock::nowVirtual()
{
    uint32_t deltaMs = millis() - _anchorMillis;
    return _anchorUtc + static_cast<time_t>(deltaMs / 1000UL);
}

// -----------------------------------------------------------------------------
// Recalage depuis source fiable (RTC ou NTP)
// -----------------------------------------------------------------------------
void VirtualClock::sync(time_t utc)
{
    _anchorUtc      = utc;
    _anchorMillis   = millis();
    _VClockSynced   = true;

    Console::info(TAG, "VirtualClock mise à jour : " + formatLocalTime(utc));
}

// -----------------------------------------------------------------------------
// L'horloge a-t-elle été recalée au moins une fois ?
// -----------------------------------------------------------------------------
bool VirtualClock::isVClockSynced()
{
    return _VClockSynced;
}