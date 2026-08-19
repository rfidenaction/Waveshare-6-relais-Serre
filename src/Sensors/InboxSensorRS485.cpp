// Sensors/InboxSensorRS485.cpp
// Lecture du capteur air boîtier Ebyte KTH2-R (adresse 15)
// via RS485 Modbus RTU sur Serial1.
//
// Même protocole que AirSensorRS485 (Holding Registers, fonction 0x03) :
//   - 0x0300 : température (valeur × 0.1 °C, signée)
//   - 0x0301 : humidité (valeur × 0.1 %RH)
// Capteur configuré à 4800 bauds (voir Web/WebServer.cpp, section Ebyte).
//
// Architecture lecture / publication :
//   readHardware() tourne à RS485_TEMP_READ_PERIOD_MS pour la surveillance
//   thermique du boîtier. publishValues() n'est appelée que :
//     - à la première lecture réussie
//     - toutes les heures (battement fixe, jamais recalé)
//     - sur température excessive (en supplément, sans affecter le battement)
//   Chaque lecture est en outre offerte à ConditionalWatering, qui publiera
//   lui-même la mesure si elle déclenche un arrosage.

#include "Sensors/InboxSensorRS485.h"
#include "Config/TimingConfig.h"
#include "Core/DataBus.h"
#include "Sensors/SoilSensorRS485.h"   // isMaintenanceMode() — bus RS485 partagé
#include "Gardener/ConditionalWatering.h"
#include "Connectivity/SmsManager.h"
#include "Utils/Console.h"

// ─────────────────────────────────────────────────────────────────────────────
// Constantes Modbus — capteur Ebyte KTH2-R
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint8_t  MODBUS_FN_READ      = 0x03;
static constexpr uint16_t REG_START           = 0x0300;
static constexpr uint16_t REG_COUNT           = 2;
static constexpr size_t   RESPONSE_LENGTH     = 9;          // 1+1+1+4+2 octets
static constexpr unsigned long RESPONSE_TIMEOUT_MS = 200;

// ─────────────────────────────────────────────────────────────────────────────
// État statique
// ─────────────────────────────────────────────────────────────────────────────

bool InboxSensorRS485::_initialized = false;

static bool          firstReadDone      = false;
static unsigned long lastPublishMs      = 0;

static bool          wasOverThreshold   = false;
static unsigned long lastAlertSmsMs     = 0;
static bool          alertSmsEverSent   = false;
static unsigned long lastClearSmsMs     = 0;
static bool          clearSmsEverSent   = false;

// ─────────────────────────────────────────────────────────────────────────────
// init — Serial1 déjà ouvert par SoilSensorRS485::init() (même bus, même baud)
// ─────────────────────────────────────────────────────────────────────────────

void InboxSensorRS485::init()
{
    _initialized = true;
    Console::info(TAG, "Capteur boîtier Ebyte KTH2-R — adresse "
                       + String(SENSOR_ADDRESS)
                       + " — bus RS485 partagé (Serial1 déjà ouvert par SoilSensorRS485)");
}

// ─────────────────────────────────────────────────────────────────────────────
// handle — lecture cadencée + publication découplée (1 h / front thermique)
//
// Chaque appel :
//   1. Lecture matérielle (readHardware)
//   2. Mesures offertes à l'arrosage conditionnel
//   3. Première lecture → publication + point de départ du battement
//   4. Front thermique montant (>= seuil) → SMS alerte (cooldown 24 h) + publish
//   5. Front thermique descendant (< seuil − offset) → SMS fin d'alerte (cooldown 24 h) + publish
//   6. Battement horaire → publication périodique (jamais recalé)
//   7. Sinon : valeurs lues, pas de publication
// ─────────────────────────────────────────────────────────────────────────────

void InboxSensorRS485::handle()
{
    if (!_initialized) return;
    if (SoilSensorRS485::isMaintenanceMode()) return;
    if (millis() < INBOX_RS485_START_DELAY_MS) return;

    float temperature = 0.0f;
    float humidity    = 0.0f;

    if (!readHardware(temperature, humidity)) return;

    // L'arrosage conditionnel travaille à la cadence de lecture, sans attendre
    // la publication horaire. Sans effet si aucune règle ne cite ces id.
    ConditionalWatering::offerMeasure(TEMPERATURE_ID, temperature);
    ConditionalWatering::offerMeasure(HUMIDITY_ID,    humidity);

    bool shouldPublish = false;

    // ── Première lecture réussie → publication initiale ──────────────────
    if (!firstReadDone) {
        firstReadDone = true;
        lastPublishMs = millis();
        shouldPublish = true;
    }

    // ── Front thermique montant → SMS alerte + publication ──────────────
    if (!wasOverThreshold && temperature >= SMS_INBOX_TEMP_THRESHOLD) {
        wasOverThreshold = true;
        shouldPublish    = true;

        if (SMS_INBOX_TEMP_ENABLED) {
            bool cooldownOk = !alertSmsEverSent ||
                              (millis() - lastAlertSmsMs >= SMS_INBOX_TEMP_COOLDOWN_MS);
            if (cooldownOk) {
                SmsManager::alert("Alerte : température boîtier "
                                  + String(temperature, 1) + " °C (seuil "
                                  + String(SMS_INBOX_TEMP_THRESHOLD, 0) + " °C)");
                lastAlertSmsMs   = millis();
                alertSmsEverSent = true;
            }
        }
    }

    // ── Front thermique descendant → SMS fin d'alerte + publication ─────
    float clearThreshold = SMS_INBOX_TEMP_THRESHOLD - SMS_INBOX_TEMP_CLEAR_OFFSET;

    if (wasOverThreshold && temperature < clearThreshold) {
        wasOverThreshold = false;
        shouldPublish    = true;

        if (SMS_INBOX_TEMP_ENABLED) {
            bool cooldownOk = !clearSmsEverSent ||
                              (millis() - lastClearSmsMs >= SMS_INBOX_TEMP_COOLDOWN_MS);
            if (cooldownOk) {
                SmsManager::alert("Fin d'alerte : température boîtier "
                                  + String(temperature, 1) + " °C (seuil retour "
                                  + String(clearThreshold, 0) + " °C)");
                lastClearSmsMs   = millis();
                clearSmsEverSent = true;
            }
        }
    }

    // ── Battement horaire → publication périodique ───────────────────────
    if (firstReadDone && (millis() - lastPublishMs >= INBOX_RS485_PUBLISH_PERIOD_MS)) {
        shouldPublish = true;
        lastPublishMs = millis();
    }

    if (shouldPublish) {
        publishValues(temperature, humidity);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// publishValues — publication des deux valeurs sur DataBus
// ─────────────────────────────────────────────────────────────────────────────

void InboxSensorRS485::publishValues(float temperature, float humidity)
{
    BusItem item = {};

    item.type       = getMeta(TEMPERATURE_ID).type;
    item.id         = TEMPERATURE_ID;
    item.valueKind  = 0;
    item.valueFloat = temperature;
    DataBus::publish(item);

    item.type       = getMeta(HUMIDITY_ID).type;
    item.id         = HUMIDITY_ID;
    item.valueKind  = 0;
    item.valueFloat = humidity;
    DataBus::publish(item);

    Console::info(TAG, "Publication — Capteur @" + String(SENSOR_ADDRESS)
                      + " — Température : " + String(temperature, 1) + " °C"
                      + "  |  Humidité : " + String(humidity, 1) + " %");
}

// ─────────────────────────────────────────────────────────────────────────────
// readHardware — transaction Modbus pure (aucune publication, aucun effet de bord)
//
// Lit les registres 0x0300 (température) et 0x0301 (humidité) du capteur
// Ebyte KTH2-R à l'adresse SENSOR_ADDRESS. Retourne true si le capteur a
// répondu correctement.
// ─────────────────────────────────────────────────────────────────────────────

bool InboxSensorRS485::readHardware(float& temperature, float& humidity)
{
    uint8_t request[8];
    request[0] = SENSOR_ADDRESS;
    request[1] = MODBUS_FN_READ;
    request[2] = (REG_START >> 8) & 0xFF;
    request[3] = REG_START & 0xFF;
    request[4] = (REG_COUNT >> 8) & 0xFF;
    request[5] = REG_COUNT & 0xFF;

    uint16_t txCrc = crc16(request, 6);
    request[6] = txCrc & 0xFF;
    request[7] = (txCrc >> 8) & 0xFF;

    drainRxBuffer();

    Serial1.write(request, sizeof(request));
    Serial1.flush();

    uint8_t response[16];
    size_t idx = 0;
    unsigned long startMs = millis();

    while (idx < RESPONSE_LENGTH && (millis() - startMs) < RESPONSE_TIMEOUT_MS) {
        if (Serial1.available()) {
            response[idx++] = Serial1.read();
        }
    }

    if (idx < RESPONSE_LENGTH) {
        Console::warn(TAG, "Pas de réponse du capteur boîtier (adresse "
                           + String(SENSOR_ADDRESS) + ") — reçu "
                           + String(idx) + "/" + String(RESPONSE_LENGTH) + " octets");
        return false;
    }

    uint16_t rxCrc   = response[7] | ((uint16_t)response[8] << 8);
    uint16_t calcCrc = crc16(response, 7);

    if (rxCrc != calcCrc) {
        Console::debug(TAG, "CRC invalide — reçu 0x" + String(rxCrc, HEX)
                             + ", calculé 0x" + String(calcCrc, HEX));
        return false;
    }

    if (response[0] != SENSOR_ADDRESS) {
        Console::debug(TAG, "Adresse inattendue : " + String(response[0]));
        return false;
    }

    if (response[1] != MODBUS_FN_READ) {
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

    int16_t rawTemp = (int16_t)(((uint16_t)response[3] << 8) | response[4]);
    temperature = rawTemp / 10.0f;

    uint16_t rawHumidity = ((uint16_t)response[5] << 8) | response[6];
    humidity = rawHumidity / 10.0f;

    if (temperature == 0.0f && humidity == 0.0f) {
        Console::warn(TAG, "Capteur @" + String(SENSOR_ADDRESS)
                          + " : valeurs 0/0 suspectes — lecture ignorée");
        return false;
    }

    Console::info(TAG, "Capteur @" + String(SENSOR_ADDRESS)
                      + " — Température : " + String(temperature, 1) + " °C"
                      + "  |  Humidité : " + String(humidity, 1) + " %");

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Mesure à la demande — déclaration des DataId produits et exécution ponctuelle
// ─────────────────────────────────────────────────────────────────────────────

uint8_t InboxSensorRS485::measurableCount()
{
    return 2;
}

DataId InboxSensorRS485::measurableAt(uint8_t index)
{
    return (index == 1) ? HUMIDITY_ID : TEMPERATURE_ID;
}

bool InboxSensorRS485::measureNow(DataId id)
{
    if (!_initialized) return false;

    if (id != TEMPERATURE_ID && id != HUMIDITY_ID) return false;

    if (SoilSensorRS485::isMaintenanceMode()) {
        Console::warn(TAG, "Mesure à la demande refusée — mode maintenance actif");
        return false;
    }

    if (millis() < INBOX_RS485_START_DELAY_MS) {
        Console::warn(TAG, "Mesure à la demande refusée — délai de démarrage "
                           "du bus RS485 non écoulé");
        return false;
    }

    float temperature = 0.0f;
    float humidity    = 0.0f;

    if (!readHardware(temperature, humidity)) return false;

    publishValues(temperature, humidity);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// drainRxBuffer — purge des octets résiduels dans le buffer RX
// ─────────────────────────────────────────────────────────────────────────────

void InboxSensorRS485::drainRxBuffer()
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

uint16_t InboxSensorRS485::crc16(const uint8_t* data, size_t len)
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
