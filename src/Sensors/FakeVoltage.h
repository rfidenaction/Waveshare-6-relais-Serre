// Sensors/FakeVoltage.h
// Simulateur de tension d'alimentation (sinusoïde 20-30V)
// Usage : test du pipeline DataLogger → SPIFFS → Web
// À SUPPRIMER en production
#pragma once

#include <Arduino.h>

class FakeVoltage {
public:
    static void init();
    static void handle();   // Appelé toutes les 30s par TaskManager

private:
    static uint16_t counter;   // 0 → 359 (degrés conceptuels)
};