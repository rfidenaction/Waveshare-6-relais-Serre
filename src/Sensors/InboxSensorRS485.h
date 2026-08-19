// Sensors/InboxSensorRS485.h
// Lecture du capteur air boîtier Ebyte KTH2-R (adresse 15)
// via RS485 Modbus RTU sur Serial1 (UART1 isolé, bus partagé avec
// SoilSensorRS485, AirSensorRS485 et SupplyVoltage — direction auto hardware).
//
// Un seul capteur, pas de rotation : chaque appel de handle() interroge
// l'adresse 15 et stocke AirTemperature1 + AirHumidity1.
//
// Architecture lecture / publication :
//   La lecture matérielle tourne à RS485_TEMP_READ_PERIOD_MS — cadence plancher
//   commune aux capteurs de température — pour surveiller la température du
//   boîtier (alerte SMS si température excessive, seuil et cooldown dans
//   SmsManager.h).
//   La publication sur DataBus est découplée :
//     - à la première lecture réussie
//     - toutes les heures (INBOX_RS485_PUBLISH_PERIOD_MS, battement fixe)
//     - immédiatement sur température excessive
//     - immédiatement si la mesure déclenche un arrosage conditionnel
//       (publication faite par ConditionalWatering, pas par ce module)
//
// Ce module est séparé d'AirSensorRS485 pour avoir son propre timing dans
// TimingConfig, indépendant des capteurs air « généraux » (adresses 13, 14).
#pragma once

#include <Arduino.h>
#include "Config/MetaDataModel.h"

class InboxSensorRS485 {
public:
    static void init();

    // Lecture périodique + publication découplée + alerte température.
    // Appelé périodiquement par TaskManager (RS485_TEMP_READ_PERIOD_MS :
    // un seul capteur, pas de rotation, donc pas de division d'effectif).
    // Bloquant ~200 ms max (timeout Modbus).
    // Inactif si SoilSensorRS485::isMaintenanceMode() est true (bus partagé).
    static void handle();

    // ─── Mesure à la demande ─────────────────────────────────────────────
    // Même contrat que AirSensorRS485 et SoilSensorRS485 : ce module déclare
    // les DataId qu'il produit, OnDemandMeasure agrège au démarrage.

    static uint8_t measurableCount();
    static DataId  measurableAt(uint8_t index);

    // Interroge immédiatement le capteur et publie la paire sur DataBus.
    // Appelée depuis le thread TaskManager uniquement (bus RS485 partagé).
    static bool measureNow(DataId id);

private:
    static constexpr const char* TAG = "InboxRS485";

    static constexpr uint8_t SENSOR_ADDRESS   = 15;
    static constexpr DataId  TEMPERATURE_ID   = DataId::AirTemperature1;
    static constexpr DataId  HUMIDITY_ID      = DataId::AirHumidity1;

    static bool _initialized;

    // Transaction Modbus pure : lecture température + humidité, pas de
    // publication ni d'effet de bord. Retourne true si le capteur a répondu.
    static bool readHardware(float& temperature, float& humidity);

    // Publication des deux valeurs sur DataBus (AirTemperature1 + AirHumidity1).
    static void publishValues(float temperature, float humidity);

    static void drainRxBuffer();
    static uint16_t crc16(const uint8_t* data, size_t len);
};
