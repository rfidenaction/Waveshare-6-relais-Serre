// Sensors/SoilSensorRS485.h
// Lecture des sondes de sol ZTS-3000-TR-WS-N01 (Liyuan Electronic)
// via RS485 Modbus RTU sur Serial1 (UART1 isolé, direction auto hardware).
//
// 6 capteurs sur le bus, interrogés en rotation (un par appel de handle()).
// Chaque capteur fournit humidité sol (%) et température sol (°C),
// publiés sur DataBus après validation.
#pragma once

#include <Arduino.h>
#include "Config/MetaDataModel.h"

class SoilSensorRS485 {
public:
    static void init();

    // Interroge UN capteur (rotation) et publie sur DataBus.
    // Appelé périodiquement par TaskManager (SOIL_RS485_HANDLE_PERIOD_MS).
    // Bloquant ~100 ms max (timeout Modbus).
    // Inactif si _maintenanceMode est true.
    static void handle();

    // ─── Mode maintenance (programmation d'adresse via page web) ─────────
    // Quand actif, handle() est inhibé et Serial1 est réservé aux
    // opérations de scan/écriture lancées depuis le thread AsyncTCP.

    static void setMaintenanceMode(bool on);
    static bool isMaintenanceMode();

    // Cherche l'unique capteur présent sur le bus (adresses 1..15).
    // Retourne l'adresse trouvée, ou 0 si aucun capteur ne répond.
    // Bloquant ~3 s max (15 × 200 ms timeout).
    static uint8_t findCurrentAddress();

    // Écrit une nouvelle adresse dans le capteur.
    // Envoie la trame Modbus 0x06 sur le registre 0x07D0.
    // Retourne true si l'écho de confirmation est correct.
    static bool setAddress(uint8_t currentAddr, uint8_t newAddr);

private:
    static constexpr const char* TAG = "RS485";

    struct SensorDescriptor {
        uint8_t address;
        DataId  moistureId;
        DataId  temperatureId;
    };

    static constexpr uint8_t SENSOR_COUNT = 6;
    static constexpr SensorDescriptor SENSORS[SENSOR_COUNT] = {
        { 0x01, DataId::SoilMoisture1, DataId::SoilTemperature1 },
        { 0x02, DataId::SoilMoisture2, DataId::SoilTemperature2 },
        { 0x03, DataId::SoilMoisture3, DataId::SoilTemperature3 },
        { 0x04, DataId::SoilMoisture4, DataId::SoilTemperature4 },
        { 0x05, DataId::SoilMoisture5, DataId::SoilTemperature5 },
        { 0x06, DataId::SoilMoisture6, DataId::SoilTemperature6 },
    };

    static bool    _initialized;
    static bool    _maintenanceMode;
    static uint8_t _currentSensor;

    static bool readSensor(uint8_t address, float& moisture, float& temperature);
    static void drainRxBuffer();
    static uint16_t crc16(const uint8_t* data, size_t len);
};