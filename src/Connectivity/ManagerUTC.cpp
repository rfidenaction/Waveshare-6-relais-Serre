// Connectivity/ManagerUTC.cpp
// Agent NTP — obtient l'heure et la transmet à RTCManager + VirtualClock
// Ce module ne fournit AUCUNE API de lecture du temps.

#include "Connectivity/ManagerUTC.h"
#include "Core/RTCManager.h"
#include "Core/VirtualClock.h"
#include <WiFi.h>
#include <time.h>
#include "lwip/apps/sntp.h"
#include "Config/Config.h"
#include "Utils/Console.h"

static const char* TAG = "NTP";

// ─────────────────────────────────────────────
// Paramètres temporels
// ─────────────────────────────────────────────

static constexpr uint32_t NETWORK_STABLE_DELAY_MS   = 60UL * 1000UL;               // 1 min
static constexpr uint32_t BOOT_RETRY_INTERVAL_MS    = 30UL * 1000UL;               // 30 s
static constexpr uint8_t  BOOT_MAX_ATTEMPTS         = 10;

static constexpr uint32_t RESYNC_PERIOD_MS          = 24UL * 60UL * 60UL * 1000UL; // 24 h

static constexpr time_t   UTC_MIN_VALID_TIMESTAMP   = 1700000000; // ~2023

// ─────────────────────────────────────────────
// Helper : formater un time_t en heure locale
// ─────────────────────────────────────────────
static String formatLocalTime(time_t utc)
{
    setenv("TZ", SYSTEM_TIMEZONE, 1);
    tzset();
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
// État interne
// ─────────────────────────────────────────────

bool     ManagerUTC::_everSynced       = false;
uint32_t ManagerUTC::_networkUpSinceMs = 0;
uint32_t ManagerUTC::_lastAttemptMs    = 0;
uint32_t ManagerUTC::_lastSyncMs       = 0;
uint8_t  ManagerUTC::_bootAttempts     = 0;

// ─────────────────────────────────────────────
// Initialisation
// ─────────────────────────────────────────────

void ManagerUTC::init()
{
    _everSynced       = false;
    _networkUpSinceMs = 0;
    _lastAttemptMs    = 0;
    _lastSyncMs       = 0;
    _bootAttempts     = 0;

    sntp_stop();

    Console::info(TAG, "Agent NTP initialisé (resync 24h)");
}

// ─────────────────────────────────────────────
// Boucle autonome
// ─────────────────────────────────────────────

void ManagerUTC::handle()
{
    const uint32_t nowMs = millis();

    // ─── Gestion état réseau ──────────────────
    if (WiFi.status() == WL_CONNECTED) {
        if (_networkUpSinceMs == 0) {
            _networkUpSinceMs = nowMs;
            _lastAttemptMs    = 0;
            _bootAttempts     = 0;
        }
    } else {
        _networkUpSinceMs = 0;
        return;
    }

    // Réseau pas encore stable (1 min)
    if (nowMs - _networkUpSinceMs < NETWORK_STABLE_DELAY_MS) {
        return;
    }

    // ─── Jamais synchronisé : tentatives rapides au boot ─────
    if (!_everSynced) {

        if (_bootAttempts >= BOOT_MAX_ATTEMPTS) {
            if (nowMs - _lastAttemptMs < RESYNC_PERIOD_MS) {
                return;
            }
        } else {
            if (nowMs - _lastAttemptMs < BOOT_RETRY_INTERVAL_MS) {
                return;
            }
        }

        _lastAttemptMs = nowMs;
        _bootAttempts++;

        if (trySync()) {
            _everSynced = true;
            _lastSyncMs = nowMs;
        }

        return;
    }

    // ─── Déjà synchronisé : resync toutes les 24h ─────
    if (nowMs - _lastSyncMs >= RESYNC_PERIOD_MS) {
        _lastAttemptMs = nowMs;
        if (trySync()) {
            _lastSyncMs = nowMs;
        }
    }
}

// ─────────────────────────────────────────────
// Synchronisation NTP
// Succès → écriture RTC + recalage VirtualClock
// ─────────────────────────────────────────────

bool ManagerUTC::trySync()
{
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    configTzTime(SYSTEM_TIMEZONE, "pool.ntp.org", "time.nist.gov", "europe.pool.ntp.org");

    sntp_init();

    const uint32_t startMs = millis();
    time_t utcNow = 0;

    while (millis() - startMs < 10000) {
        time(&utcNow);
        if (utcNow >= UTC_MIN_VALID_TIMESTAMP) {
            sntp_stop();

            Console::info(TAG, "Synchro NTP réussie : " + formatLocalTime(utcNow)
                + " (essai " + String(_bootAttempts)
                + ", " + String((millis() - _networkUpSinceMs) / 1000)
                + "s après WiFi)");

            // Écriture dans le RTC DS3231
            RTCManager::write(utcNow);

            // Recalage VirtualClock
            VirtualClock::sync(utcNow);

            return true;
        }
        delay(100);
    }

    sntp_stop();
    Console::warn(TAG, "Tentative NTP échouée (timeout 10s)");
    return false;
}