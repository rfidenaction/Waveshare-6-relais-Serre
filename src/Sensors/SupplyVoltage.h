// Sensors/SupplyVoltage.h
// Lecture de la tension d'alimentation via la carte Waveshare Analog Input 8CH (B)
// sur le bus RS485 (Modbus RTU, adresse 16, canal 1).
//
// L'entrée analogique reçoit la tension d'alimentation divisée par 4
// (pont résistif). La valeur lue est multipliée par 4 pour reconstituer
// la tension réelle.
#pragma once

#include <Arduino.h>

class SupplyVoltage {
public:
    static void init();
    static void handle();   // Appelé périodiquement par TaskManager

private:
    static uint16_t crc16(const uint8_t* data, size_t len);
};
