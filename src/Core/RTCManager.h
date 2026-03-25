// Core/RTCManager.h
// Pilote matériel DS3231
// Carte : Pico-RTC-DS3231 sur Waveshare ESP32-S3-Relay-6CH
// I2C : GPIO4 (SDA), GPIO5 (SCL)
//
// Aucune variable d'état persistante.
// read() vérifie OSF + lecture + validation à chaque appel.
// write() vérifie la présence du chip par ping I2C avant écriture.
#pragma once

#include <Arduino.h>
#include <time.h>

class RTCManager {
public:
    // Initialisation : Wire.begin(4,5) + détection DS3231 + log OSF
    static void init();

    // Lecture DS3231 : OSF + lecture + validation
    // true → rtcOut contient le temps UTC lu
    // false → rtcOut pas touché, lecture échouée ou OSF actif
    static bool read(time_t& rtcOut);

    // Écriture DS3231 : ping I2C + adjust + vérification OSF
    static bool write(time_t utc);
};