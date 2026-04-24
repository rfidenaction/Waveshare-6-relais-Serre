// Core/RTCManager.h
// Pilote matériel DS3231
// Carte : Pico-RTC-DS3231 sur Waveshare ESP32-S3-Relay-6CH
// I2C : GPIO4 (SDA), GPIO5 (SCL)
//
// Aucune variable d'état persistante.
// read() vérifie OSF + lecture + validation à chaque appel.
// write() vérifie la présence du chip par ping I2C avant écriture.
//
// Flag "chip mort" en RAM (non persistant) :
//   Levé uniquement quand une écriture laisse OSF à 1 après coup — signe
//   d'un vrai problème matériel. Une fois levé, read() et write()
//   retournent false immédiatement, sans toucher au bus I2C, jusqu'au
//   prochain reboot.
#pragma once

#include <Arduino.h>
#include <time.h>

class RTCManager {
public:
    // Initialisation : Wire.begin(4,5) + détection DS3231 + log OSF
    static void init();

    // Lecture DS3231 : chip mort ? + OSF + lecture + validation
    // true → rtcOut contient le temps UTC lu
    // false → rtcOut pas touché (chip mort, OSF actif ou lecture invalide)
    static bool read(time_t& rtcOut);

    // Écriture DS3231 : chip mort ? + ping I2C + adjust + vérification OSF
    // Lève _chipDead si OSF reste à 1 après écriture.
    static bool write(time_t utc);

private:
    static bool _chipDead;   // RAM, reset au reboot
};