// Sensors/SupplyVoltage.cpp
// Lecture de la tension d'alimentation via Waveshare Analog Input 8CH (B)
// sur le bus RS485 (Modbus RTU).
//
// La carte est à l'adresse 16, configurée à 4800 bauds, mode 0 (0–10V).
// Le canal 1 (registre 0x0000) porte la tension d'alimentation divisée
// par 4 via un pont résistif. La valeur brute est en millivolts.
//
// Protocole : fonction 0x04 (Read Input Registers), 1 registre.

#include "Sensors/SupplyVoltage.h"
#include "Sensors/SoilSensorRS485.h"   // isMaintenanceMode()
#include "Config/TimingConfig.h"       // SOIL_RS485_START_DELAY_MS
#include "Core/DataBus.h"
#include "Utils/Console.h"

static const char* TAG = "SupplyVoltage";

// ─────────────────────────────────────────────────────────────────────────────
// Constantes Modbus — Waveshare Analog Input 8CH (B)
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint8_t  DEVICE_ADDRESS        = 16;        // 0x10
static constexpr uint8_t  MODBUS_FN_READ_INPUT  = 0x04;      // Read Input Registers
static constexpr uint16_t REG_CHANNEL_1         = 0x0000;
static constexpr uint16_t REG_COUNT             = 1;
static constexpr size_t   RESPONSE_LENGTH       = 7;          // addr+fn+byteCount+2data+2crc
static constexpr unsigned long RESPONSE_TIMEOUT_MS = 200;

static constexpr float RESISTOR_DIVIDER_RATIO   = 4.0f;

// ─────────────────────────────────────────────────────────────────────────────
// init
// ─────────────────────────────────────────────────────────────────────────────

void SupplyVoltage::init()
{
    Console::info(TAG, "Lecture tension alim — adresse "
                       + String(DEVICE_ADDRESS) + ", canal 1, diviseur x"
                       + String((int)RESISTOR_DIVIDER_RATIO));
}

// ─────────────────────────────────────────────────────────────────────────────
// handle — lecture périodique du canal 1
// ─────────────────────────────────────────────────────────────────────────────

void SupplyVoltage::handle()
{
    if (SoilSensorRS485::isMaintenanceMode()) return;
    if (millis() < SOIL_RS485_START_DELAY_MS) return;

    // ── Purge buffer RX ──────────────────────────────────────────────────
    while (Serial1.available()) {
        Serial1.read();
    }

    // ── Construction de la requête ───────────────────────────────────────
    uint8_t request[8];
    request[0] = DEVICE_ADDRESS;
    request[1] = MODBUS_FN_READ_INPUT;
    request[2] = (REG_CHANNEL_1 >> 8) & 0xFF;
    request[3] = REG_CHANNEL_1 & 0xFF;
    request[4] = (REG_COUNT >> 8) & 0xFF;
    request[5] = REG_COUNT & 0xFF;

    uint16_t txCrc = crc16(request, 6);
    request[6] = txCrc & 0xFF;
    request[7] = (txCrc >> 8) & 0xFF;

    // ── Envoi ────────────────────────────────────────────────────────────
    Serial1.write(request, sizeof(request));
    Serial1.flush();

    // ── Réception ────────────────────────────────────────────────────────
    uint8_t response[16];
    size_t idx = 0;
    unsigned long startMs = millis();

    while (idx < RESPONSE_LENGTH && (millis() - startMs) < RESPONSE_TIMEOUT_MS) {
        if (Serial1.available()) {
            response[idx++] = Serial1.read();
        }
    }

    if (idx < RESPONSE_LENGTH) {
        Console::warn(TAG, "Pas de réponse de la carte Analog Input (adresse "
                           + String(DEVICE_ADDRESS) + ") — reçu "
                           + String(idx) + "/" + String(RESPONSE_LENGTH) + " octets");
        return;
    }

    // ── Validation CRC ───────────────────────────────────────────────────
    uint16_t rxCrc   = response[5] | ((uint16_t)response[6] << 8);
    uint16_t calcCrc = crc16(response, 5);

    if (rxCrc != calcCrc) {
        Console::warn(TAG, "CRC invalide — reçu 0x" + String(rxCrc, HEX)
                           + ", calculé 0x" + String(calcCrc, HEX));
        return;
    }

    // ── Validation en-tête ───────────────────────────────────────────────
    if (response[0] != DEVICE_ADDRESS) {
        Console::warn(TAG, "Adresse inattendue : " + String(response[0]));
        return;
    }

    if (response[1] != MODBUS_FN_READ_INPUT) {
        if (response[1] & 0x80) {
            Console::warn(TAG, "Exception Modbus — code " + String(response[2]));
        } else {
            Console::warn(TAG, "Code fonction inattendu : 0x" + String(response[1], HEX));
        }
        return;
    }

    if (response[2] != REG_COUNT * 2) {
        Console::warn(TAG, "Byte count inattendu : " + String(response[2]));
        return;
    }

    // ── Décodage ─────────────────────────────────────────────────────────
    uint16_t rawMV = ((uint16_t)response[3] << 8) | response[4];

    float inputVoltage  = rawMV / 1000.0f;
    float supplyVoltage = inputVoltage * RESISTOR_DIVIDER_RATIO;

    BusItem item = {};
    item.type       = getMeta(DataId::SupplyVoltage).type;
    item.id         = DataId::SupplyVoltage;
    item.valueKind  = 0;
    item.valueFloat = supplyVoltage;
    DataBus::publish(item);

    Console::info(TAG, "Brut=" + String(rawMV) + " mV"
                       + "  |  Entrée=" + String(inputVoltage, 3) + " V"
                       + "  |  Alim=" + String(supplyVoltage, 1) + " V");
}

// ─────────────────────────────────────────────────────────────────────────────
// crc16 — CRC16 Modbus RTU
// ─────────────────────────────────────────────────────────────────────────────

uint16_t SupplyVoltage::crc16(const uint8_t* data, size_t len)
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
