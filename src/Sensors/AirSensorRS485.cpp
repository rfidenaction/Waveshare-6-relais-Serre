// Sensors/AirSensorRS485.cpp
// Lecture des capteurs air Ebyte KTH2-R via RS485 Modbus RTU sur Serial1.
//
// Registres Modbus (Holding Registers, fonction 0x03) :
//   - 0x0300 : température (lecture seule, valeur × 0.1 °C, signée)
//   - 0x0301 : humidité (lecture seule, valeur × 0.1 %RH)
// Capteurs configurés à 4800 bauds (voir Web/WebServer.cpp, section Ebyte).

#include "AirSensorRS485.h"
#include "Config/TimingConfig.h"       // AIR_RS485_START_DELAY_MS
#include "Core/DataBus.h"
#include "Sensors/SoilSensorRS485.h"   // isMaintenanceMode() — bus RS485 partagé
#include "Utils/Console.h"

// ─────────────────────────────────────────────────────────────────────────────
// Constantes Modbus — capteur Ebyte KTH2-R
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint8_t  MODBUS_FN_READ      = 0x03;       // Read Holding Registers
static constexpr uint16_t REG_START           = 0x0300;     // Température puis humidité
static constexpr uint16_t REG_COUNT           = 2;
static constexpr size_t   RESPONSE_LENGTH     = 9;          // 1+1+1+4+2 octets
static constexpr unsigned long RESPONSE_TIMEOUT_MS = 200;   // Timeout réponse

// ─────────────────────────────────────────────────────────────────────────────
// État statique
// ─────────────────────────────────────────────────────────────────────────────

bool    AirSensorRS485::_initialized   = false;
uint8_t AirSensorRS485::_currentSensor = 0;

// ─────────────────────────────────────────────────────────────────────────────
// init — Serial1 déjà ouvert par SoilSensorRS485::init() (même bus, même baud)
// ─────────────────────────────────────────────────────────────────────────────

void AirSensorRS485::init()
{
    _initialized   = true;
    _currentSensor = 0;
    Console::info(TAG, "Capteurs Ebyte KTH2-R — bus RS485 partagé (Serial1 déjà ouvert par SoilSensorRS485) — "
                       + String(SENSOR_COUNT) + " capteur(s) en rotation");
}

// ─────────────────────────────────────────────────────────────────────────────
// handle — interrogation périodique du capteur
// ─────────────────────────────────────────────────────────────────────────────

void AirSensorRS485::handle()
{
    if (!_initialized) return;
    if (SoilSensorRS485::isMaintenanceMode()) return;
    if (millis() < AIR_RS485_START_DELAY_MS) return;

    const SensorDescriptor& sensor = SENSORS[_currentSensor];

    float temperature = 0.0f;
    float humidity    = 0.0f;

    if (readSensor(sensor.address, temperature, humidity)) {

        if (temperature == 0.0f && humidity == 0.0f) {
            Console::warn(TAG, "Capteur @" + String(sensor.address)
                              + " : valeurs 0/0 suspectes — publication ignorée");
        } else {
            BusItem item = {};

            item.type       = getMeta(sensor.temperatureId).type;
            item.id         = sensor.temperatureId;
            item.valueKind  = 0;
            item.valueFloat = temperature;
            DataBus::publish(item);

            item.type       = getMeta(sensor.humidityId).type;
            item.id         = sensor.humidityId;
            item.valueKind  = 0;
            item.valueFloat = humidity;
            DataBus::publish(item);

            Console::info(TAG, "Capteur @" + String(sensor.address)
                              + " — Température : " + String(temperature, 1) + " °C"
                              + "  |  Humidité : " + String(humidity, 1) + " %");
        }
    } else {
        Console::warn(TAG, "Pas de réponse du capteur air (adresse "
                           + String(sensor.address) + ")");
    }

    _currentSensor = (_currentSensor + 1) % SENSOR_COUNT;
}

// ─────────────────────────────────────────────────────────────────────────────
// readSensor — transaction Modbus RTU complète (envoi + réception + décodage)
//
// Trame TX (8 octets) :
//   [addr] [0x03] [regH] [regL] [cntH] [cntL] [crcL] [crcH]
//
// Trame RX attendue (9 octets) :
//   [addr] [0x03] [byteCount=4] [tempH] [tempL] [humH] [humL] [crcL] [crcH]
// ─────────────────────────────────────────────────────────────────────────────

bool AirSensorRS485::readSensor(uint8_t address, float& temperature, float& humidity)
{
    // ── Construction de la requête ──────────────────────────────────────────

    uint8_t request[8];
    request[0] = address;
    request[1] = MODBUS_FN_READ;
    request[2] = (REG_START >> 8) & 0xFF;   // registre départ, octet haut
    request[3] = REG_START & 0xFF;           // registre départ, octet bas
    request[4] = (REG_COUNT >> 8) & 0xFF;   // nombre de registres, octet haut
    request[5] = REG_COUNT & 0xFF;           // nombre de registres, octet bas

    uint16_t txCrc = crc16(request, 6);
    request[6] = txCrc & 0xFF;              // CRC bas en premier (convention Modbus)
    request[7] = (txCrc >> 8) & 0xFF;       // CRC haut

    // ── Envoi ───────────────────────────────────────────────────────────────

    drainRxBuffer();

    Serial1.write(request, sizeof(request));
    Serial1.flush();    // attend la fin de l'émission physique

    // ── Réception ───────────────────────────────────────────────────────────

    uint8_t response[16];
    size_t idx = 0;
    unsigned long startMs = millis();

    while (idx < RESPONSE_LENGTH && (millis() - startMs) < RESPONSE_TIMEOUT_MS) {
        if (Serial1.available()) {
            response[idx++] = Serial1.read();
        }
    }

    if (idx < RESPONSE_LENGTH) {
        Console::debug(TAG, "Timeout — reçu " + String(idx) + "/" + String(RESPONSE_LENGTH) + " octets");
        return false;
    }

    // ── Validation CRC ──────────────────────────────────────────────────────

    uint16_t rxCrc   = response[7] | ((uint16_t)response[8] << 8);
    uint16_t calcCrc = crc16(response, 7);

    if (rxCrc != calcCrc) {
        Console::debug(TAG, "CRC invalide — reçu 0x" + String(rxCrc, HEX)
                             + ", calculé 0x" + String(calcCrc, HEX));
        return false;
    }

    // ── Validation en-tête ──────────────────────────────────────────────────

    if (response[0] != address) {
        Console::debug(TAG, "Adresse inattendue : " + String(response[0]));
        return false;
    }

    if (response[1] != MODBUS_FN_READ) {
        // Bit 7 à 1 = exception Modbus
        if (response[1] & 0x80) {
            Console::warn(TAG, "Exception Modbus — code " + String(response[2]));
        } else {
            Console::debug(TAG, "Code fonction inattendu : 0x" + String(response[1], HEX));
        }
        return false;
    }

    if (response[2] != REG_COUNT * 2) {
        Console::debug(TAG, "Byte count inattendu : " + String(response[2]));
        return false;
    }

    // ── Décodage des registres ──────────────────────────────────────────────

    // Registre 0x0300 — température air (valeur × 10, complément à deux si < 0 °C)
    int16_t rawTemp = (int16_t)(((uint16_t)response[3] << 8) | response[4]);
    temperature = rawTemp / 10.0f;

    // Registre 0x0301 — humidité air (valeur × 10, ex : 658 → 65,8 %)
    uint16_t rawHumidity = ((uint16_t)response[5] << 8) | response[6];
    humidity = rawHumidity / 10.0f;

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// drainRxBuffer — purge des octets résiduels dans le buffer RX
// ─────────────────────────────────────────────────────────────────────────────

void AirSensorRS485::drainRxBuffer()
{
    while (Serial1.available()) {
        Serial1.read();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// crc16 — CRC16 Modbus RTU
//
// Polynôme : 0xA001 (bit-reversed de 0x8005)
// Valeur initiale : 0xFFFF
// ─────────────────────────────────────────────────────────────────────────────

uint16_t AirSensorRS485::crc16(const uint8_t* data, size_t len)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc;
}
