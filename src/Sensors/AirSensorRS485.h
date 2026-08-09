// Sensors/AirSensorRS485.h
// Lecture des capteurs air température/humidité Ebyte KTH2-R
// via RS485 Modbus RTU sur Serial1 (UART1 isolé, bus partagé avec
// SoilSensorRS485 et SupplyVoltage — direction auto hardware).
//
// Les capteurs sur le bus, interrogés en rotation (un par appel de handle()).
// Chaque capteur fournit température air (°C) et humidité air (%),
// publiés sur DataBus après lecture.
//
// La programmation d'adresse/baud rate des capteurs Ebyte est gérée par
// ailleurs (Web/WebServer.cpp, handlers /rs485/read-ebyte et /rs485/program-ebyte).
// Ce module se limite à l'interrogation périodique en régime normal.
#pragma once

#include <Arduino.h>
#include "Config/MetaDataModel.h"

class AirSensorRS485 {
public:
    static void init();

    // Interroge UN capteur (rotation) et publie sur DataBus.
    // Appelé périodiquement par TaskManager (AIR_RS485_HANDLE_PERIOD_MS).
    // Bloquant ~100 ms max (timeout Modbus).
    // Inactif si SoilSensorRS485::isMaintenanceMode() est true (bus partagé).
    static void handle();

private:
    static constexpr const char* TAG = "AirRS485";

    struct SensorDescriptor {
        uint8_t address;
        DataId  temperatureId;
        DataId  humidityId;
    };

    static constexpr uint8_t SENSOR_COUNT = 3;
    static constexpr SensorDescriptor SENSORS[SENSOR_COUNT] = {
        { 13, DataId::AirTemperature3, DataId::AirHumidity3 },
        { 14, DataId::AirTemperature2, DataId::AirHumidity2 },
        { 15, DataId::AirTemperature1, DataId::AirHumidity1 },
    };

    static bool    _initialized;
    static uint8_t _currentSensor;

    static bool readSensor(uint8_t address, float& temperature, float& humidity);
    static void drainRxBuffer();
    static uint16_t crc16(const uint8_t* data, size_t len);
};
