// Sensors/SupplyVoltage.cpp
// Lecture de la tension d'alimentation via Waveshare Analog Input 8CH (B)
// sur le bus RS485 (Modbus RTU).
//
// La carte est à l'adresse 16, configurée à 4800 bauds, mode 0 (0–10V).
// Le canal 1 (registre 0x0000) porte la tension d'alimentation divisée
// par 4 via un pont résistif. La valeur brute est en millivolts.
//
// Le canal 2 (registre 0x0001) porte une tension indicatrice de la
// présence du secteur. Les deux canaux sont lus en une seule transaction.
//
// Protocole : fonction 0x04 (Read Input Registers), 2 registres.
//
// Architecture lecture / publication :
//   La lecture matérielle (readHardware) tourne à cadence rapide
//   (SUPPLY_VOLTAGE_HANDLE_PERIOD_MS, typ. 30 s) pour détecter sans délai
//   un front secteur et déclencher l'alerte SMS.
//   La publication sur DataBus (publishValues) est découplée :
//     - à la première lecture réussie (valeur visible dès le boot)
//     - toutes les heures ensuite (SUPPLY_VOLTAGE_PUBLISH_PERIOD_MS)
//     - sur front secteur, en supplément du battement, seulement si le
//       cooldown d'une heure (alerte et retour séparés) est écoulé
//   Ce découplage permet de garder la réactivité SMS en production sans
//   inonder DataBus / MQTT / CSV à chaque tick de 30 s.

#include "Sensors/SupplyVoltage.h"
#include "Sensors/SoilSensorRS485.h"   // isMaintenanceMode()
#include "Config/TimingConfig.h"
#include "Core/DataBus.h"
#include "Connectivity/SmsManager.h"
#include "Utils/Console.h"

static const char* TAG = "SupplyVoltage";

// ─────────────────────────────────────────────────────────────────────────────
// Constantes Modbus — Waveshare Analog Input 8CH (B)
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint8_t  DEVICE_ADDRESS        = 16;        // 0x10
static constexpr uint8_t  MODBUS_FN_READ_INPUT  = 0x04;      // Read Input Registers
static constexpr uint16_t REG_CHANNEL_1         = 0x0000;
static constexpr uint16_t REG_COUNT             = 2;          // canal 1 + canal 2
static constexpr size_t   RESPONSE_LENGTH       = 9;          // addr+fn+byteCount+4data+2crc
static constexpr unsigned long RESPONSE_TIMEOUT_MS = 200;

static constexpr float RESISTOR_DIVIDER_RATIO   = 4.06f;

// ─────────────────────────────────────────────────────────────────────────────
// État interne — dernières valeurs lues et suivi de publication
// ─────────────────────────────────────────────────────────────────────────────

static float         lastVoltage       = 0.0f;
static float         lastAcPower       = 1.0f;
static bool          firstReadDone     = false;
static unsigned long lastPublishMs     = 0;

static unsigned long lastAlertSmsMs    = 0;
static bool          alertSmsEverSent  = false;
static unsigned long lastClearSmsMs    = 0;
static bool          clearSmsEverSent  = false;

// ─────────────────────────────────────────────────────────────────────────────
// init
// ─────────────────────────────────────────────────────────────────────────────

void SupplyVoltage::init()
{
    Console::info(TAG, "Lecture tension alim — adresse "
                       + String(DEVICE_ADDRESS) + ", canal 1, diviseur x"
                       + String(RESISTOR_DIVIDER_RATIO, 2));
}

// ─────────────────────────────────────────────────────────────────────────────
// handle — lecture rapide (30 s) + publication découplée (1 h / front)
//
// Chaque appel :
//   1. Lecture matérielle (readHardware)
//   2. Front secteur → SMS + publication extra, si cooldown écoulé
//   3. Battement horaire → publication périodique
//   4. Sinon : valeurs stockées, pas de publication
// ─────────────────────────────────────────────────────────────────────────────

void SupplyVoltage::handle()
{
    if (SoilSensorRS485::isMaintenanceMode()) return;
    if (millis() < SOIL_RS485_START_DELAY_MS) return;

    float voltage = 0.0f;
    float acPower = 0.0f;

    if (!readHardware(voltage, acPower)) return;

    bool shouldPublish = false;

    // ── Première lecture réussie → publication initiale ──────────────────
    if (!firstReadDone) {
        firstReadDone  = true;
        lastPublishMs  = millis();
        shouldPublish  = true;
    }

    // ── Front secteur → SMS + publication extra, même cooldown 1 h ──────
    // Le battement horaire n'est pas concerné. Alerte et fin d'alerte restent
    // indépendantes. lastAcPower est mis à jour plus bas dans tous les cas :
    // un front refusé n'est pas rejoué, le suivant attend un nouveau changement.
    if (acPower != lastAcPower) {
        if (acPower == 0.0f) {
            bool cooldownOk = !alertSmsEverSent ||
                              (millis() - lastAlertSmsMs >= SMS_AC_POWER_COOLDOWN_MS);
            if (cooldownOk) {
                lastAlertSmsMs   = millis();
                alertSmsEverSent = true;
                shouldPublish    = true;
                if (SMS_AC_POWER_ENABLED) {
                    SmsManager::alert("Alerte : coupure secteur 220V");
                }
            }
        } else {
            bool cooldownOk = !clearSmsEverSent ||
                              (millis() - lastClearSmsMs >= SMS_AC_POWER_COOLDOWN_MS);
            if (cooldownOk) {
                lastClearSmsMs   = millis();
                clearSmsEverSent = true;
                shouldPublish    = true;
                if (SMS_AC_POWER_ENABLED) {
                    SmsManager::alert("Fin d'alerte : retour secteur 220V");
                }
            }
        }
    }

    // ── Battement horaire → publication périodique ───────────────────────
    bool heartbeat = firstReadDone &&
                     (millis() - lastPublishMs >= SUPPLY_VOLTAGE_PUBLISH_PERIOD_MS);
    if (heartbeat) {
        shouldPublish  = true;
        lastPublishMs  = millis();
    }

    // ── Mise à jour de l'état interne (avant publication éventuelle) ────
    lastVoltage = voltage;
    lastAcPower = acPower;

    if (shouldPublish) {
        publishValues(voltage, acPower);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// publishValues — publication des deux canaux sur DataBus
// ─────────────────────────────────────────────────────────────────────────────

void SupplyVoltage::publishValues(float voltage, float acPower)
{
    BusItem item = {};
    item.type       = getMeta(DataId::SupplyVoltage).type;
    item.id         = DataId::SupplyVoltage;
    item.valueKind  = 0;
    item.valueFloat = voltage;
    DataBus::publish(item);

    BusItem item2 = {};
    item2.type       = getMeta(DataId::AcPower).type;
    item2.id         = DataId::AcPower;
    item2.valueKind  = 0;
    item2.valueFloat = acPower;
    DataBus::publish(item2);

    Console::info(TAG, "Publication — Alim=" + String(voltage, 1) + " V"
                       + "  |  AcPower=" + String(acPower == 1.0f ? "Présent" : "Absent"));
}

// ─────────────────────────────────────────────────────────────────────────────
// readHardware — transaction Modbus pure (aucune publication, aucun effet de bord)
//
// Lit les deux canaux de la carte Analog Input 8CH et décode :
//   - canal 1 → tension d'alimentation (après multiplication par le diviseur)
//   - canal 2 → état secteur (1.0 si >= 4 V, 0.0 sinon)
// Retourne true si la carte a répondu correctement.
// ─────────────────────────────────────────────────────────────────────────────

bool SupplyVoltage::readHardware(float& voltage, float& acPower)
{
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
        return false;
    }

    // ── Validation CRC ───────────────────────────────────────────────────
    uint16_t rxCrc   = response[7] | ((uint16_t)response[8] << 8);
    uint16_t calcCrc = crc16(response, 7);

    if (rxCrc != calcCrc) {
        Console::warn(TAG, "CRC invalide — reçu 0x" + String(rxCrc, HEX)
                           + ", calculé 0x" + String(calcCrc, HEX));
        return false;
    }

    // ── Validation en-tête ───────────────────────────────────────────────
    if (response[0] != DEVICE_ADDRESS) {
        Console::warn(TAG, "Adresse inattendue : " + String(response[0]));
        return false;
    }

    if (response[1] != MODBUS_FN_READ_INPUT) {
        if (response[1] & 0x80) {
            Console::warn(TAG, "Exception Modbus — code " + String(response[2]));
        } else {
            Console::warn(TAG, "Code fonction inattendu : 0x" + String(response[1], HEX));
        }
        return false;
    }

    if (response[2] != REG_COUNT * 2) {
        Console::warn(TAG, "Byte count inattendu : " + String(response[2]));
        return false;
    }

    // ── Décodage canal 1 — tension d'alimentation ─────────────────────
    uint16_t rawMV_ch1 = ((uint16_t)response[3] << 8) | response[4];
    float inputVoltage = rawMV_ch1 / 1000.0f;
    voltage = inputVoltage * RESISTOR_DIVIDER_RATIO;

    Console::info(TAG, "CH1 Brut=" + String(rawMV_ch1) + " mV"
                       + "  |  Entrée=" + String(inputVoltage, 3) + " V"
                       + "  |  Alim=" + String(voltage, 1) + " V");

    // ── Décodage canal 2 — détection secteur ────────────────────────
    uint16_t rawMV_ch2 = ((uint16_t)response[5] << 8) | response[6];
    float mainsVoltage = rawMV_ch2 / 1000.0f;
    acPower = (mainsVoltage >= 4.0f) ? 1.0f : 0.0f;

    Console::info(TAG, "CH2 Brut=" + String(rawMV_ch2) + " mV"
                       + "  |  Tension=" + String(mainsVoltage, 3) + " V"
                       + "  |  AcPower=" + String(acPower == 1.0f ? "Présent" : "Absent"));

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Mesure à la demande — déclaration des DataId produits et exécution ponctuelle
//
// Les deux ids sont écrits ici : un seul appareil physique sur le bus, une
// seule transaction, deux canaux. OnDemandMeasure les récupère au démarrage
// pour construire sa vue id → propriétaire.
//
// Hors de handle() : pas de détection de front, pas de SMS, pas de cooldown.
// Un appui UI publie toujours, même pendant l'heure qui suit une alerte.
// ─────────────────────────────────────────────────────────────────────────────

uint8_t SupplyVoltage::measurableCount()
{
    return 2;
}

DataId SupplyVoltage::measurableAt(uint8_t index)
{
    return (index == 1) ? DataId::AcPower : DataId::SupplyVoltage;
}

bool SupplyVoltage::measureNow(DataId id)
{
    if (id != DataId::SupplyVoltage && id != DataId::AcPower) return false;

    if (SoilSensorRS485::isMaintenanceMode()) {
        Console::warn(TAG, "Mesure à la demande refusée — mode maintenance actif");
        return false;
    }

    if (millis() < SOIL_RS485_START_DELAY_MS) {
        Console::warn(TAG, "Mesure à la demande refusée — délai de démarrage "
                           "du bus RS485 non écoulé");
        return false;
    }

    float voltage = 0.0f;
    float acPower = 0.0f;

    if (!readHardware(voltage, acPower)) return false;

    publishValues(voltage, acPower);
    return true;
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
