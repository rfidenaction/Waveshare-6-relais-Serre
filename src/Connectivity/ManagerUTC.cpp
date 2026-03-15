// Connectivity/ManagerUTC.cpp

#include "Connectivity/ManagerUTC.h"
#include <WiFi.h>
#include <time.h>
#include "lwip/apps/sntp.h"
#include "Config/Config.h"
#include "Utils/Console.h"

// ─────────────────────────────────────────────
// Paramètres temporels (validés)
// ─────────────────────────────────────────────

static constexpr uint32_t NETWORK_STABLE_DELAY_MS   = 60UL * 1000UL;        // 1 min
static constexpr uint32_t BOOT_RETRY_INTERVAL_MS    = 30UL * 1000UL;        // 30 s
static constexpr uint8_t  BOOT_MAX_ATTEMPTS         = 10;

static constexpr uint32_t RESYNC_PERIOD_MS          = 3UL * 60UL * 60UL * 1000UL; // 3 h
static constexpr uint32_t EXPIRED_RETRY_PERIOD_MS   = 60UL * 60UL * 1000UL;      // 1 h
static constexpr uint32_t UTC_EXPIRATION_MS         = 25UL * 60UL * 60UL * 1000UL; // 25 h

static constexpr time_t   UTC_MIN_VALID_TIMESTAMP   = 1700000000; // ~2023

// ─────────────────────────────────────────────
// État interne
// ─────────────────────────────────────────────

bool     ManagerUTC::utcValid          = false;
bool     ManagerUTC::everSynced        = false;

uint32_t ManagerUTC::networkUpSinceMs  = 0;
uint32_t ManagerUTC::lastAttemptMs     = 0;
uint32_t ManagerUTC::lastSyncMs        = 0;

uint8_t  ManagerUTC::bootAttempts      = 0;

uint32_t ManagerUTC::syncRelMs         = 0;
time_t   ManagerUTC::syncUtc           = 0;

// ─────────────────────────────────────────────
// Initialisation
// ─────────────────────────────────────────────

void ManagerUTC::init()
{
    utcValid         = false;
    everSynced       = false;

    networkUpSinceMs = 0;
    lastAttemptMs    = 0;
    lastSyncMs       = 0;

    bootAttempts     = 0;

    syncRelMs        = 0;
    syncUtc          = 0;

    // On s'assure que SNTP est arrêté au démarrage
    sntp_stop();
}

// ─────────────────────────────────────────────
// Boucle autonome
// ─────────────────────────────────────────────

void ManagerUTC::handle()
{
    const uint32_t nowMs = millis();

    // ─── Gestion état réseau ──────────────────
    if (WiFi.status() == WL_CONNECTED) {
        if (networkUpSinceMs == 0) {
            networkUpSinceMs = nowMs;
            lastAttemptMs    = 0;
            bootAttempts     = 0;
        }
    } else {
        networkUpSinceMs = 0;
        return;
    }

    // Réseau pas encore stable (1 min)
    if (nowMs - networkUpSinceMs < NETWORK_STABLE_DELAY_MS) {
        return;
    }

    // ─── UTC invalide ─────────────────────────
    if (!utcValid) {

        uint32_t retryInterval =
            (everSynced || bootAttempts >= BOOT_MAX_ATTEMPTS)
                ? EXPIRED_RETRY_PERIOD_MS
                : BOOT_RETRY_INTERVAL_MS;

        if (nowMs - lastAttemptMs >= retryInterval) {
            lastAttemptMs = nowMs;

            if (!everSynced) {
                bootAttempts++;
            }

            if (trySync()) {
                utcValid   = true;
                everSynced = true;
                lastSyncMs = nowMs;
            }
        }

        return;
    }

    // ─── UTC valide : resync toutes les 3h ─────
    if (nowMs - lastSyncMs >= RESYNC_PERIOD_MS) {
        if (trySync()) {
            lastSyncMs = nowMs;
        }
    }

    // ─── Expiration après 25h sans resync ─────
    if (nowMs - lastSyncMs >= UTC_EXPIRATION_MS) {
        utcValid = false;
    }
}

// ─────────────────────────────────────────────
// API publique
// ─────────────────────────────────────────────

bool ManagerUTC::isUtcValid()
{
    return utcValid;
}

time_t ManagerUTC::nowUtc()
{
    if (!utcValid) return 0;

    uint32_t deltaMs = millis() - syncRelMs;
    return syncUtc + static_cast<time_t>(deltaMs / 1000UL);
}

time_t ManagerUTC::convertFromRelative(uint32_t t_rel_ms)
{
    if (!utcValid) return 0;

    int32_t deltaMs = static_cast<int32_t>(t_rel_ms - syncRelMs);
    return syncUtc + static_cast<time_t>(deltaMs / 1000L);
}

// ─────────────────────────────────────────────
// Synchronisation NTP contrôlée (low-data)
// ─────────────────────────────────────────────

bool ManagerUTC::trySync()
{
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    // Configuration des serveurs NTP
    configTzTime(SYSTEM_TIMEZONE, "pool.ntp.org", "time.nist.gov", "europe.pool.ntp.org");

    // Démarrage manuel du client SNTP
    sntp_init();

    const uint32_t startMs = millis();
    time_t utcNow = 0;

    // On laisse jusqu'à 10 secondes pour recevoir une réponse valide
    while (millis() - startMs < 10000) {
        time(&utcNow);
        if (utcNow >= UTC_MIN_VALID_TIMESTAMP) {
            sntp_stop();  // Arrêt immédiat pour limiter les émissions

            syncRelMs = millis();
            syncUtc   = utcNow;

            Console::info("[UTC] Synchro NTP réussie après " + 
                String(bootAttempts) + 
                " essai(s) au boot (temps : " + 
                String((millis() - networkUpSinceMs) / 1000) + " s depuis connexion WiFi)");
            
            return true;
        }
        delay(100);
    }

    // Timeout → arrêt propre
    sntp_stop();
    return false;
}