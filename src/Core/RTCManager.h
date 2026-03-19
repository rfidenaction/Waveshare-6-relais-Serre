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

    // Présent ET oscillateur n'a pas été interrompu (pile ok)
    static bool isReliable();

    // Lecture directe DS3231 → time_t UTC (0 si absent)
    static time_t read();

    // Écriture DS3231 (efface OSF automatiquement via RTClib::adjust)
    static bool write(time_t utc);

    // Conversion timestamp relatif (millis) → UTC
    // Utilise le temps RTC courant comme référence
    static time_t convertFromRelative(uint32_t t_rel_ms);

private:
    static bool _present;
    static bool _reliable;
};