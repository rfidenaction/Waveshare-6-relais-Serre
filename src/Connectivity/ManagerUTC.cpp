// Connectivity/ManagerUTC.cpp
// Maître du temps + Agent NTP
// readUTC() : point d'accès unique au temps (cascade RTC → VClock → millis)
// handle()  : agent NTP (synchro périodique)

#include "Connectivity/ManagerUTC.h"
#include "Core/RTCManager.h"
#include "Core/VirtualClock.h"
#include <WiFi.h>
#include <time.h>
#include "lwip/apps/sntp.h"
#include "Config/Config.h"
#include "Config/TimingConfig.h"
#include "Utils/Console.h"

static const char* TAG = "NTP";

// ─────────────────────────────────────────────
// Paramètres temporels
// ─────────────────────────────────────────────

static constexpr uint32_t NETWORK_STABLE_DELAY_MS   = 60UL * 1000UL;               // 1 min
static constexpr uint32_t BOOT_RETRY_INTERVAL_MS    = 30UL * 1000UL;               // 30 s
static constexpr uint8_t  BOOT_MAX_ATTEMPTS         = 10;

static constexpr time_t   UTC_MIN_VALID_TIMESTAMP   = 1700000000; // ~2023

// ─────────────────────────────────────────────
// Helper : formater un time_t en heure locale
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
// État interne
// ─────────────────────────────────────────────

bool     ManagerUTC::_everSynced       = false;
uint32_t ManagerUTC::_networkUpSinceMs = 0;
uint32_t ManagerUTC::_lastAttemptMs    = 0;
uint32_t ManagerUTC::_lastTourMs       = 0;
uint8_t  ManagerUTC::_bootAttempts     = 0;
uint8_t  ManagerUTC::_tourCount        = 0;

ManagerUTC::NtpState ManagerUTC::_ntpState    = NtpState::IDLE;
uint32_t             ManagerUTC::_ntpStartMs  = 0;

// ─────────────────────────────────────────────
// Initialisation
// ─────────────────────────────────────────────

void ManagerUTC::init()
{
    _everSynced       = false;
    _networkUpSinceMs = 0;
    _lastAttemptMs    = 0;
    _lastTourMs       = 0;
    _bootAttempts     = 0;
    _tourCount        = 0;
    _ntpState         = NtpState::IDLE;
    _ntpStartMs       = 0;

    sntp_stop();

    Console::info(TAG, "Agent NTP initialisé");
}

// ─────────────────────────────────────────────
// readUTC — Point d'accès unique au temps
//
// Cascade :
//   1. RTC OK (ping + lecture)   → UTC fiable
//   2. VClock synced             → UTC approximatif
//   3. Rien                      → millis()
//
// Ne retourne JAMAIS 0 dans timestamp.
// ─────────────────────────────────────────────

TimeUTC ManagerUTC::readUTC()
{
    TimeUTC t;

    // Cas 1 : RTC matériel (OSF clair + lecture OK + timestamp valide)
    time_t rtcTime;
    if (RTCManager::read(rtcTime)) {
        t.timestamp     = rtcTime;
        t.UTC_available = true;
        t.UTC_reliable  = true;
        return t;
    }

    // Cas 2 : VirtualClock recalée (UTC approximatif, dérive sur millis)
    if (VirtualClock::isVClockSynced()) {
        t.timestamp     = VirtualClock::nowVirtual();
        t.UTC_available = true;
        t.UTC_reliable  = false;
        return t;
    }

    // Cas 3 : aucun UTC — millis() comme filet de sécurité
    t.timestamp     = static_cast<time_t>(millis());
    t.UTC_available = false;
    t.UTC_reliable  = false;
    return t;
}

// ─────────────────────────────────────────────
// Boucle autonome
// ─────────────────────────────────────────────

void ManagerUTC::handle()
{
    const uint32_t nowMs = millis();

    // ─── Tentative NTP en cours → vérifier le résultat ───
    if (_ntpState == NtpState::WAITING) {
        checkNtp();
        return;
    }

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

    // ─── Phase boot : tentatives rapides (10 essais, 30s) ─────
    if (!_everSynced && _bootAttempts < BOOT_MAX_ATTEMPTS) {

        if (nowMs - _lastAttemptMs < BOOT_RETRY_INTERVAL_MS) {
            return;
        }

        _lastAttemptMs = nowMs;
        _bootAttempts++;
        startNtp();

        return;
    }

    // ─── Phase routine : un essai par tour de 50 min ─────
    if (nowMs - _lastTourMs < NTP_RETRY_PERIOD_MS) {
        return;
    }
    _lastTourMs = nowMs;

    if (!VirtualClock::isVClockSynced()) {
        // VClock pas synced → essai à chaque tour
        startNtp();
    } else {
        // VClock synced → essai tous les NTP_ROUTINE_TOUR_COUNT tours
        _tourCount++;
        if (_tourCount >= NTP_ROUTINE_TOUR_COUNT) {
            _tourCount = 0;
            startNtp();
        }
    }
}

// ─────────────────────────────────────────────
// startNtp — Lance une tentative NTP (non-bloquant)
// ─────────────────────────────────────────────

void ManagerUTC::startNtp()
{
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    configTzTime(SYSTEM_TIMEZONE, "pool.ntp.org", "time.nist.gov", "europe.pool.ntp.org");
    sntp_init();

    _ntpStartMs = millis();
    _ntpState   = NtpState::WAITING;

    Console::info(TAG, "Tentative NTP lancée");
}

// ─────────────────────────────────────────────
// checkNtp — Vérifie si le NTP a répondu (appelé par handle)
// Succès → écriture RTC + recalage VirtualClock
// Timeout → abandon, retour en IDLE
// ─────────────────────────────────────────────

void ManagerUTC::checkNtp()
{
    time_t utcNow = 0;
    time(&utcNow);

    if (utcNow >= UTC_MIN_VALID_TIMESTAMP) {
        // ── Succès ──
        sntp_stop();
        _ntpState = NtpState::IDLE;

        Console::info(TAG, "Synchro NTP réussie : " + formatLocalTime(utcNow)
            + " (essai " + String(_bootAttempts)
            + ", " + String((millis() - _networkUpSinceMs) / 1000)
            + "s après WiFi)");

        RTCManager::write(utcNow);
        VirtualClock::sync(utcNow);

        _everSynced = true;
        _lastTourMs = millis();
        return;
    }

    // ── Timeout (10s) ──
    if (millis() - _ntpStartMs >= 10000) {
        sntp_stop();
        _ntpState = NtpState::IDLE;

        Console::info(TAG, "Tentative NTP échouée (timeout 10s)");
    }
}