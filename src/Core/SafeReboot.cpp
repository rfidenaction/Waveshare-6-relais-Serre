// Core/SafeReboot.cpp
// Reboot préventif automatique
//
// Séquence :
//  1. Au boot → init() : _rebootDeadline = 0
//  2. Premier handle() (~5 min après boot) :
//     - Lit readUTC()
//     - UTC disponible → calcule le délai jusqu'au prochain 1er à 12h25 locale
//     - UTC indisponible → fallback 45 jours - 5 min
//     - Stocke la deadline en µs (esp_timer)
//  3. Handles suivants (toutes les 5 min) :
//     - Compare esp_timer_get_time() à la deadline
//     - Si atteinte → flush PENDING, log, reboot

#include "Core/SafeReboot.h"
#include "Connectivity/ManagerUTC.h"
#include "Storage/DataLogger.h"
#include "Config/Config.h"
#include "Config/TimingConfig.h"
#include "Utils/Console.h"

#include <esp_timer.h>
#include <time.h>

static const char* TAG = "SafeReboot";

// État interne
int64_t SafeReboot::_rebootDeadline = 0;

// -----------------------------------------------------------------------------
// Helper : calculer le délai (en secondes) jusqu'au prochain 1er du mois
// à l'heure cible en heure locale France.
//
// Logique :
//  - Construit la date cible (1er du mois courant, heure configurée)
//  - Si cette date est déjà passée → avance au mois suivant
//  - mktime() normalise automatiquement (décembre+1 → janvier année suivante)
//  - tm_isdst = -1 → mktime() détermine le DST automatiquement
// -----------------------------------------------------------------------------
static int64_t calcDelaySeconds(time_t nowUtc)
{
    setenv("TZ", SYSTEM_TIMEZONE, 1);
    tzset();

    struct tm local;
    localtime_r(&nowUtc, &local);

    // Cible : 1er du mois courant à l'heure configurée
    struct tm target = local;
    target.tm_mday  = SAFE_REBOOT_TARGET_DAY;
    target.tm_hour  = SAFE_REBOOT_TARGET_HOUR;
    target.tm_min   = SAFE_REBOOT_TARGET_MINUTE;
    target.tm_sec   = 0;
    target.tm_isdst = -1;   // laisser mktime déterminer le DST

    time_t targetUtc = mktime(&target);

    // Si la cible est passée ou maintenant → mois suivant
    if (targetUtc <= nowUtc) {
        target.tm_mon  += 1;    // mktime normalise (13 → janvier année+1)
        target.tm_isdst = -1;
        targetUtc = mktime(&target);
    }

    return (int64_t)(targetUtc - nowUtc);
}

// -----------------------------------------------------------------------------
// Helper : formater un délai en secondes pour le log
// -----------------------------------------------------------------------------
static String formatDelay(int64_t delaySec)
{
    int jours  = (int)(delaySec / 86400);
    int heures = (int)((delaySec % 86400) / 3600);
    int mins   = (int)((delaySec % 3600) / 60);
    return String(jours) + "j " + String(heures) + "h " + String(mins) + "m";
}

// -----------------------------------------------------------------------------
// Helper : formater un time_t en date/heure locale pour le log
// -----------------------------------------------------------------------------
static String formatTargetDate(time_t utc)
{
    setenv("TZ", SYSTEM_TIMEZONE, 1);
    tzset();
    struct tm tmLocal;
    localtime_r(&utc, &tmLocal);

    char buf[32];
    snprintf(buf, sizeof(buf), "%02d/%02d/%04d %02d:%02d",
             tmLocal.tm_mday,
             tmLocal.tm_mon + 1,
             tmLocal.tm_year + 1900,
             tmLocal.tm_hour,
             tmLocal.tm_min);
    return String(buf);
}

// -----------------------------------------------------------------------------
// Helper : formater le temps restant depuis la deadline esp_timer
// -----------------------------------------------------------------------------
static String formatRemaining(int64_t deadlineUs)
{
    int64_t remainUs = deadlineUs - esp_timer_get_time();
    if (remainUs < 0) return "0";
    int64_t remainSec = remainUs / 1000000LL;
    return formatDelay(remainSec);
}

// -----------------------------------------------------------------------------
// Initialisation
// -----------------------------------------------------------------------------
void SafeReboot::init()
{
    _rebootDeadline = 0;
    Console::info(TAG, "Initialisé");
}

// -----------------------------------------------------------------------------
// Handle — appelé toutes les 5 minutes par TaskManager
// -----------------------------------------------------------------------------
void SafeReboot::handle()
{
    // ─── Premier appel : calcul de la deadline ───────────────────────
    if (_rebootDeadline == 0) {
        TimeUTC t = ManagerUTC::readUTC();

        if (t.UTC_available) {
            int64_t delaySec = calcDelaySeconds(t.timestamp);
            _rebootDeadline = esp_timer_get_time() + delaySec * 1000000LL;

            // Calculer la date cible pour le log
            time_t targetUtc = t.timestamp + (time_t)delaySec;
            Console::info(TAG, "Cible : " + formatTargetDate(targetUtc)
                + " (dans " + formatDelay(delaySec) + ")");
        } else {
            // Fallback : 45 jours - 5 minutes
            _rebootDeadline = esp_timer_get_time() + SAFE_REBOOT_FALLBACK_US;
            Console::warn(TAG, "UTC indisponible — fallback 45j - 5min");
        }
        return;
    }

    // ─── Vérification deadline ───────────────────────────────────────
    if (esp_timer_get_time() >= _rebootDeadline) {

        // TODO : vérifier que toutes les vannes sont fermées
        // TODO : vérifier qu'aucun arrosage n'est en cours ou imminent

        Console::info(TAG, "Deadline atteinte — lancement séquence reboot");

        // Flush des données en attente vers SPIFFS
        Console::info(TAG, "Flush PENDING → SPIFFS");
        DataLogger::handle();

        Console::info(TAG, "Reboot automatique programmé");

        delay(200);     // laisser le temps au log de sortir sur UART
        ESP.restart();
    }

    // ─── Signe de vie (log du temps restant) ─────────────────────────
    Console::debug(TAG, "Restant : " + formatRemaining(_rebootDeadline));
}