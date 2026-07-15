// Sensors/SoilSensorRS485.cpp
// Lecture des sondes de sol ZTS-3000-TR-WS-N01 (Liyuan Electronic)
// via RS485 Modbus RTU sur Serial1.
//
// Voir Synthese_RS485_Capteur_Sol.md pour la documentation complète
// (registres, trames, câblage, procédure multi-capteurs).

#include "SoilSensorRS485.h"
#include "Config/IO-Config.h"          // RS485_TX_PIN, RS485_RX_PIN
#include "Config/TimingConfig.h"       // SOIL_RS485_START_DELAY_MS
#include "Core/DataBus.h"
#include "Utils/Console.h"

// ─────────────────────────────────────────────────────────────────────────────
// Constantes Modbus — capteur ZTS-3000-TR-WS-N01
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint32_t RS485_BAUD          = 4800;       // Défaut usine
static constexpr uint8_t  MODBUS_FN_READ      = 0x03;       // Read Holding Registers
static constexpr uint16_t REG_START           = 0x0000;     // Premier registre (humidité)
static constexpr uint16_t REG_COUNT           = 2;          // Humidité + température
static constexpr size_t   RESPONSE_LENGTH     = 9;          // 1+1+1+4+2 octets
static constexpr unsigned long RESPONSE_TIMEOUT_MS = 200;   // Timeout réponse

// ─────────────────────────────────────────────────────────────────────────────
// État statique
// ─────────────────────────────────────────────────────────────────────────────

bool     SoilSensorRS485::_initialized      = false;
bool     SoilSensorRS485::_maintenanceMode = false;
uint8_t  SoilSensorRS485::_currentSensor   = 0;

// ─────────────────────────────────────────────────────────────────────────────
// init — ouverture de Serial1 sur le port RS485 isolé
// ─────────────────────────────────────────────────────────────────────────────

void SoilSensorRS485::init()
{
    Serial1.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);

    _initialized    = true;
    _currentSensor  = 0;
    Console::info(TAG, "Serial1 ouvert — "
                       + String(RS485_BAUD) + " bauds 8N1, "
                       "TX=GPIO" + String(RS485_TX_PIN) + " "
                       "RX=GPIO" + String(RS485_RX_PIN)
                       + " — " + String(SENSOR_COUNT) + " capteur(s) en rotation");
}

// ─────────────────────────────────────────────────────────────────────────────
// handle — interrogation périodique du capteur
// ─────────────────────────────────────────────────────────────────────────────

void SoilSensorRS485::handle()
{
    if (!_initialized) return;
    if (_maintenanceMode) return;
    if (millis() < SOIL_RS485_START_DELAY_MS) return;

    const SensorDescriptor& sensor = SENSORS[_currentSensor];

    float moisture    = 0.0f;
    float temperature = 0.0f;

    if (readSensor(sensor.address, moisture, temperature)) {

        if (moisture == 0.0f && temperature == 0.0f) {
            Console::warn(TAG, "Capteur @" + String(sensor.address)
                              + " : valeurs 0/0 suspectes — publication ignorée");
        } else {
            BusItem item = {};

            item.type       = getMeta(sensor.moistureId).type;
            item.id         = sensor.moistureId;
            item.valueKind  = 0;
            item.valueFloat = moisture;
            DataBus::publish(item);

            item.type       = getMeta(sensor.temperatureId).type;
            item.id         = sensor.temperatureId;
            item.valueKind  = 0;
            item.valueFloat = temperature;
            DataBus::publish(item);

            Console::info(TAG, "Capteur @" + String(sensor.address)
                              + " — Humidité : " + String(moisture, 1) + " %"
                              + "  |  Température : " + String(temperature, 1) + " °C");
        }
    } else {
        Console::warn(TAG, "Pas de réponse du capteur sol (adresse "
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
//   [addr] [0x03] [byteCount=4] [moistH] [moistL] [tempH] [tempL] [crcL] [crcH]
// ─────────────────────────────────────────────────────────────────────────────

bool SoilSensorRS485::readSensor(uint8_t address, float& moisture, float& temperature)
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

    // Registre 0x0000 — humidité sol (valeur × 10, ex : 658 → 65,8 %)
    uint16_t rawMoisture = ((uint16_t)response[3] << 8) | response[4];
    moisture = rawMoisture / 10.0f;

    // Registre 0x0001 — température sol (valeur × 10, complément à deux si < 0 °C)
    // Le cast int16_t gère automatiquement les valeurs négatives.
    int16_t rawTemp = (int16_t)(((uint16_t)response[5] << 8) | response[6]);
    temperature = rawTemp / 10.0f;

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// drainRxBuffer — purge des octets résiduels dans le buffer RX
// ─────────────────────────────────────────────────────────────────────────────

void SoilSensorRS485::drainRxBuffer()
{
    while (Serial1.available()) {
        Serial1.read();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Mode maintenance — inhibe handle() pour libérer Serial1
// ─────────────────────────────────────────────────────────────────────────────

void SoilSensorRS485::setMaintenanceMode(bool on)
{
    _maintenanceMode = on;
    Console::info(TAG, String("Mode maintenance ") + (on ? "activé" : "désactivé"));
}

bool SoilSensorRS485::isMaintenanceMode()
{
    return _maintenanceMode;
}

// ─────────────────────────────────────────────────────────────────────────────
// findCurrentAddress — détecte l'adresse du capteur unique branché sur le bus
// ─────────────────────────────────────────────────────────────────────────────

uint8_t SoilSensorRS485::findCurrentAddress()
{
    float moisture, temperature;

    for (uint8_t addr = 1; addr <= 15; addr++) {
        if (readSensor(addr, moisture, temperature)) {
            Console::info(TAG, "Capteur trouvé à l'adresse " + String(addr));
            return addr;
        }
    }

    Console::warn(TAG, "Aucun capteur détecté (adresses 1-15)");
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// setAddress — écrit une nouvelle adresse Modbus dans le capteur
//
// Trame TX (8 octets) — Write Single Register (0x06) :
//   [addr] [0x06] [regH=0x07] [regL=0xD0] [valH] [valL] [crcL] [crcH]
//
// Le capteur renvoie un écho identique si l'écriture réussit.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint8_t  MODBUS_FN_WRITE = 0x06;
static constexpr uint16_t REG_ADDRESS     = 0x07D0;
static constexpr size_t   WRITE_RESPONSE_LENGTH = 8;

bool SoilSensorRS485::setAddress(uint8_t currentAddr, uint8_t newAddr)
{
    if (newAddr < 1 || newAddr > 254) {
        Console::warn(TAG, "setAddress — adresse cible invalide : " + String(newAddr));
        return false;
    }

    uint8_t request[8];
    request[0] = currentAddr;
    request[1] = MODBUS_FN_WRITE;
    request[2] = (REG_ADDRESS >> 8) & 0xFF;
    request[3] = REG_ADDRESS & 0xFF;
    request[4] = 0x00;
    request[5] = newAddr;

    uint16_t txCrc = crc16(request, 6);
    request[6] = txCrc & 0xFF;
    request[7] = (txCrc >> 8) & 0xFF;

    drainRxBuffer();

    Serial1.write(request, sizeof(request));
    Serial1.flush();

    uint8_t response[16];
    size_t idx = 0;
    unsigned long startMs = millis();

    while (idx < WRITE_RESPONSE_LENGTH && (millis() - startMs) < RESPONSE_TIMEOUT_MS) {
        if (Serial1.available()) {
            response[idx++] = Serial1.read();
        }
    }

    if (idx < WRITE_RESPONSE_LENGTH) {
        Console::warn(TAG, "setAddress — timeout réponse (" + String(idx) + " octets)");
        return false;
    }

    bool echoOk = (memcmp(request, response, WRITE_RESPONSE_LENGTH) == 0);

    if (echoOk) {
        Console::info(TAG, "Adresse changée : " + String(currentAddr)
                          + " → " + String(newAddr));
    } else {
        Console::warn(TAG, "setAddress — écho incorrect");
    }

    return echoOk;
}

// ─────────────────────────────────────────────────────────────────────────────
// crc16 — CRC16 Modbus RTU
//
// Polynôme : 0xA001 (bit-reversed de 0x8005)
// Valeur initiale : 0xFFFF
// ─────────────────────────────────────────────────────────────────────────────

uint16_t SoilSensorRS485::crc16(const uint8_t* data, size_t len)
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