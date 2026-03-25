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
// Initialisation
// -----------------------------------------------------------------------------
void RTCManager::init()
{
    // Init I2C sur les pins du connecteur Pico HAT
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
    Console::info(TAG, "Init I2C (SDA=" + String(RTC_SDA_PIN) + " SCL=" + String(RTC_SCL_PIN) + ")");

    // Détection du DS3231
    if (!rtc.begin()) {
        Console::error(TAG, "DS3231 non détecté sur I2C");
        return;
    }

    Console::info(TAG, "DS3231 détecté");

    // Vérification OSF (Oscillator Stop Flag)
    if (rtc.lostPower()) {
        Console::warn(TAG, "Pile HS ou RTC jamais configuré. En attente de NTP.");
    } else {
        // Lecture et affichage en heure locale
        time_t utcNow;
        if (read(utcNow)) {
            Console::info(TAG, "Pile OK — Heure RTC : " + formatLocalTime(utcNow));
        }
    }
}

// -----------------------------------------------------------------------------
// API
// -----------------------------------------------------------------------------

bool RTCManager::read(time_t& rtcOut)
{
    // OSF — l'oscillateur s'est-il arrêté ?
    // Si le bus I2C est en panne, lostPower() retourne true (mode sûr)
    if (rtc.lostPower()) {
        return false;
    }

    // Lecture du temps
    DateTime now = rtc.now();
    time_t utc = now.unixtime();

    // Garde : valeur aberrante (timestamp pré-2023)
    if (utc < 1700000000) {
        return false;
    }

    rtcOut = utc;
    return true;
}

bool RTCManager::write(time_t utc)
{
    // Ping I2C — vérification que le DS3231 est présent
    Wire.beginTransmission(0x68);
    if (Wire.endTransmission() != 0) {
        Console::error(TAG, "Écriture impossible — DS3231 ne répond pas");
        return false;
    }

    DateTime dt(utc);
    rtc.adjust(dt);

    // Vérification OSF après écriture
    if (rtc.lostPower()) {
        Console::error(TAG, "OSF toujours actif après écriture — problème matériel");
        return false;
    }

    Console::info(TAG, "RTC mise à jour : " + formatLocalTime(utc));

    return true;
}