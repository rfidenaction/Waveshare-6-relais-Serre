// Core/RTCManager.cpp
// Maître du temps — lecture directe DS3231
#include "Core/RTCManager.h"
#include "Config/IO-Config.h"
#include "Config/Config.h"
#include "Utils/Console.h"

#include <SPI.h>        // Requis avant RTClib sous PlatformIO (issue Adafruit BusIO)
#include <Wire.h>
#include <RTClib.h>

static const char* TAG = "RTC";

// Instance RTClib (statique fichier)
static RTC_DS3231 rtc;

// État interne
bool RTCManager::_present  = false;
bool RTCManager::_reliable = false;

// -----------------------------------------------------------------------------
// Helper : formater un time_t en heure locale France (via SYSTEM_TIMEZONE)
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// Initialisation
// -----------------------------------------------------------------------------
void RTCManager::init()
{
    _present  = false;
    _reliable = false;

    // Init I2C sur les pins du connecteur Pico HAT
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
    Console::info(TAG, "Init I2C (SDA=" + String(RTC_SDA_PIN) + " SCL=" + String(RTC_SCL_PIN) + ")");

    // Détection du DS3231
    if (!rtc.begin()) {
        Console::error(TAG, "DS3231 non détecté sur I2C");
        return;
    }

    _present = true;
    Console::info(TAG, "DS3231 détecté");

    // Vérification OSF (Oscillator Stop Flag)
    if (rtc.lostPower()) {
        _reliable = false;
        Console::warn(TAG, "Pile HS ou RTC jamais configuré. En attente de NTP.");
    } else {
        _reliable = true;

        // Lecture et affichage en heure locale
        time_t utcNow = read();
        Console::info(TAG, "Pile OK — Heure RTC : " + formatLocalTime(utcNow));
    }
}

// -----------------------------------------------------------------------------
// API
// -----------------------------------------------------------------------------

bool RTCManager::isPresent()
{
    return _present;
}

bool RTCManager::isReliable()
{
    return _reliable;
}

time_t RTCManager::read()
{
    if (!_present) return 0;

    DateTime now = rtc.now();
    return now.unixtime();
}

bool RTCManager::write(time_t utc)
{
    if (!_present) {
        Console::error(TAG, "Écriture impossible — DS3231 absent");
        return false;
    }

    DateTime dt(utc);
    rtc.adjust(dt);     // adjust() efface automatiquement le flag OSF

    _reliable = true;   // Après écriture NTP, le RTC est fiable

    Console::info(TAG, "RTC mise à jour : " + formatLocalTime(utc));

    return true;
}

// -----------------------------------------------------------------------------
// Conversion relatif → UTC
// Calcul : utc_event = utc_now - (millis_now - millis_event) / 1000
// -----------------------------------------------------------------------------
time_t RTCManager::convertFromRelative(uint32_t t_rel_ms)
{
    if (!_reliable) return 0;

    time_t nowUtc = read();
    if (nowUtc == 0) return 0;

    int32_t deltaMs = static_cast<int32_t>(t_rel_ms - millis());
    return nowUtc + static_cast<time_t>(deltaMs / 1000L);
}