// Core/RTCManager.h
// Maître du temps — lecture directe DS3231
// Carte : Pico-RTC-DS3231 sur Waveshare ESP32-S3-Relay-6CH
// I2C : GPIO4 (SDA), GPIO5 (SCL)
#pragma once

#include <Arduino.h>
#include <time.h>

class RTCManager {
public:
    // Initialisation : Wire.begin(4,5) + détection DS3231 + lecture OSF
    static void init();

    // Le chip DS3231 a-t-il répondu sur le bus I2C ?
    static bool isPresent();

    // Présent ET oscillateur n'a pas été interrompu (OSF clair)
    static bool is_RTC_available();

    // Lecture DS3231 : ping I2C + lecture + vérification valeur
    // true → rtcOut contient le temps UTC lu
    // false → rtcOut pas touché, lecture échouée
    static bool read(time_t& rtcOut);

    // Écriture DS3231 via RTClib::adjust, puis vérification OSF
    // Écrit même si _RTC_available est false (on vérifie après)
    static bool write(time_t utc);

private:
    static bool _present;
    static bool _RTC_available;
};