// Core/VirtualClock.cpp
// Horloge virtuelle machine — source unique du temps runtime
#include "Core/VirtualClock.h"
#include "Core/RTCManager.h"
#include "Config/TimingConfig.h"
#include "Config/Config.h"
#include "Utils/Console.h"

static const char* TAG = "VClock";

// Ancre par défaut : 2025-01-01 11:30:00 UTC = 12h30 CET.
// Choisie pour rester cohérente avec SafeReboot (reboot mensuel à 12h25
// heure locale) : l'écart max entre horloge fictive et heure réelle est
// d'environ 5 minutes si le RTC est mort et que NTP n'arrive jamais.
static constexpr time_t DEFAULT_ANCHOR_UTC = 1735731000UL;

// ─────────────────────────────────────────────
// État interne
// ─────────────────────────────────────────────
time_t   VirtualClock::_anchorUtc       = 0;
uint32_t VirtualClock::_anchorMillis    = 0;
uint32_t VirtualClock::_lastSyncMillis  = 0;
uint32_t VirtualClock::_lastRtcResyncMs = 0;
bool     VirtualClock::_available       = false;
bool     VirtualClock::_reliable        = false;

// ─────────────────────────────────────────────
// Helper : formater un time_t en heure locale France (via SYSTEM_TIMEZONE)
// ─────────────────────────────────────────────
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

// ─────────────────────────────────────────────
// Initialisation
// ─────────────────────────────────────────────
void VirtualClock::init()
{
    _anchorUtc       = 0;
    _anchorMillis    = 0;
    _lastSyncMillis  = 0;
    _lastRtcResyncMs = 0;
    _available       = false;
    _reliable        = false;

    Console::info(TAG, "Initialisée — en attente de NTP ou RTC (T+4 min)");
}

// ─────────────────────────────────────────────
// Recalage depuis source fiable
// Appelée par NTPManager sur succès, et par handle() lors de la bascule
// initiale ou d'une resync RTC réussie.
// ─────────────────────────────────────────────
void VirtualClock::sync(time_t utc)
{
    _anchorUtc      = utc;
    _anchorMillis   = millis();
    _lastSyncMillis = _anchorMillis;
    _available      = true;
    _reliable       = true;

    Console::info(TAG, "Synchronisée : " + formatLocalTime(utc));
}

// ─────────────────────────────────────────────
// Lecture unique du temps
// VClock_available=false → timestamp = (time_t)millis() (phase pré-bascule)
// VClock_available=true  → timestamp = UTC (secondes Unix)
// ─────────────────────────────────────────────
TimeVClock VirtualClock::read()
{
    TimeVClock t;

    if (!_available) {
        t.timestamp        = static_cast<time_t>(millis());
        t.VClock_available = false;
        t.VClock_reliable  = false;
        return t;
    }

    uint32_t deltaMs   = millis() - _anchorMillis;
    t.timestamp        = _anchorUtc + static_cast<time_t>(deltaMs / 1000UL);
    t.VClock_available = true;
    t.VClock_reliable  = _reliable;
    return t;
}

// ─────────────────────────────────────────────
// Boucle autonome
//  - Phase pré-bascule : ne fait rien avant T+4min. À T+4min, tente RTC.
//  - Régime permanent  : resync RTC toutes les 3h (cadence absolue),
//                        bascule _reliable=false après 24h sans sync.
// ─────────────────────────────────────────────
void VirtualClock::handle()
{
    const uint32_t nowMs = millis();

    // ─── Phase pré-bascule ───────────────────────────────────────
    if (!_available) {
        if (nowMs < VCLOCK_START_DELAY_MS) {
            return;   // NTP conserve l'exclusivité jusqu'à T+4min
        }

        time_t rtcTime;
        if (RTCManager::read(rtcTime)) {
            _anchorUtc       = rtcTime;
            _anchorMillis    = nowMs;
            _lastSyncMillis  = nowMs;
            _lastRtcResyncMs = nowMs;
            _available       = true;
            _reliable        = true;
            Console::info(TAG, "T+4min : bascule sur RTC — "
                          + formatLocalTime(rtcTime));
        } else {
            _anchorUtc       = DEFAULT_ANCHOR_UTC;
            _anchorMillis    = nowMs;
            _lastSyncMillis  = 0;          // aucune vraie sync encore
            _lastRtcResyncMs = nowMs;
            _available       = true;
            _reliable        = false;
            Console::warn(TAG, "T+4min : RTC indisponible — ancre 12h30 arbitraire");
        }
        return;
    }

    // ─── Régime permanent ───────────────────────────────────────
    // Resync RTC toutes les 3h, cadence absolue (indépendante des sync NTP)
    if (nowMs - _lastRtcResyncMs >= VCLOCK_RTC_RESYNC_PERIOD_MS) {
        _lastRtcResyncMs = nowMs;

        time_t rtcTime;
        if (RTCManager::read(rtcTime)) {
            _anchorUtc      = rtcTime;
            _anchorMillis   = nowMs;
            _lastSyncMillis = nowMs;
            _reliable       = true;
            Console::info(TAG, "Resync RTC — " + formatLocalTime(rtcTime));
        }
    }

    // Bascule _reliable=false si aucune sync depuis 24h
    if (_reliable && _lastSyncMillis != 0
        && (nowMs - _lastSyncMillis) >= VCLOCK_RELIABLE_TIMEOUT_MS) {
        _reliable = false;
        Console::warn(TAG, "Aucune sync depuis 24h — _reliable → false");
    }
}
