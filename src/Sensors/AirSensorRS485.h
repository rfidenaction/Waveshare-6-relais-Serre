// Sensors/AirSensorRS485.h
// Lecture des capteurs air température/humidité Ebyte KTH2-R
// via RS485 Modbus RTU sur Serial1 (UART1 isolé, bus partagé avec
// SoilSensorRS485 et SupplyVoltage — direction auto hardware).
//
// Les capteurs sur le bus sont interrogés en rotation, un par appel de
// handle(). Chaque capteur fournit température air (°C) et humidité air (%).
//
// Architecture lecture / publication :
//   La lecture tourne vite — chaque capteur est lu à RS485_TEMP_READ_PERIOD_MS,
//   cadence plancher imposée par l'auto-échauffement de l'élément de mesure —
//   pour que l'arrosage conditionnel reste réactif. Chaque lecture est offerte
//   à ConditionalWatering.
//   La publication sur DataBus est découplée :
//     - à la première lecture réussie de chaque capteur
//     - toutes les heures (AIR_RS485_PUBLISH_PERIOD_MS, battement par capteur)
//     - à la demande depuis l'UI (measureNow)
//     - immédiatement si la mesure déclenche un arrosage conditionnel
//       (publication faite par ConditionalWatering, pas par ce module)
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

    // Interroge UN capteur (rotation), offre ses mesures à l'arrosage
    // conditionnel, et publie sur DataBus si le battement de ce capteur est
    // échu.
    // Appelé périodiquement par TaskManager, à RS485_TEMP_READ_PERIOD_MS
    // divisé par sensorCount() : chaque capteur est ainsi lu à la cadence
    // plancher, quel que soit l'effectif.
    // Bloquant ~100 ms max (timeout Modbus).
    // Inactif si SoilSensorRS485::isMaintenanceMode() est true (bus partagé).
    static void handle();

    // Effectif de la famille, d'où main.cpp déduit la période de la tâche.
    // constexpr : la division est faite à la compilation.
    static constexpr uint8_t sensorCount() { return SENSOR_COUNT; }

    // ─── Mesure à la demande ─────────────────────────────────────────────
    // Ce module déclare les DataId qu'il produit ; il ne les reçoit d'aucune
    // table extérieure. La liste est dérivée de SENSORS[], seule source de
    // vérité de l'appartenance id ↔ adresse Modbus. OnDemandMeasure
    // l'interroge au démarrage pour construire sa vue id → propriétaire,
    // comme ValveManager se construit depuis RELAYS[].

    // Nombre de DataId produits (2 par capteur : température + humidité).
    static uint8_t measurableCount();

    // DataId numéro `index`, avec index < measurableCount().
    static DataId measurableAt(uint8_t index);

    // Interroge immédiatement le capteur portant cet id et publie la paire
    // température + humidité sur DataBus — même chemin que handle(), donc
    // même validation, même horodatage, même journalisation CSV.
    // La rotation _currentSensor n'est pas touchée.
    // Appelée depuis le thread TaskManager uniquement (bus RS485 partagé).
    // Retourne false si l'id est inconnu du module, si le bus est indisponible
    // (mode maintenance, délai de démarrage) ou si le capteur n'a pas répondu.
    static bool measureNow(DataId id);

private:
    static constexpr const char* TAG = "AirRS485";

    struct SensorDescriptor {
        uint8_t address;
        DataId  temperatureId;
        DataId  humidityId;
    };

    static constexpr uint8_t SENSOR_COUNT = 2;
    static_assert(SENSOR_COUNT > 0,
                  "SENSOR_COUNT divise RS485_TEMP_READ_PERIOD_MS dans main.cpp");

    static constexpr SensorDescriptor SENSORS[SENSOR_COUNT] = {
        { 13, DataId::AirTemperature3, DataId::AirHumidity3 },
        { 14, DataId::AirTemperature2, DataId::AirHumidity2 },
    };

    static bool    _initialized;
    static uint8_t _currentSensor;

    // Battement de publication, propre à chaque capteur et posé à sa première
    // lecture réussie, jamais recalé ensuite. La rotation étale donc les
    // publications des capteurs dans l'heure sans qu'aucun décalage soit calculé.
    static unsigned long _lastPublishMs[SENSOR_COUNT];
    static bool          _firstPublishDone[SENSOR_COUNT];

    // Interroge le capteur décrit par `sensor` : transaction Modbus et contrôle
    // de vraisemblance, sans rien publier. Chemin de lecture commun à
    // l'acquisition périodique et à la mesure à la demande — un seul endroit
    // décide de ce qui est rejeté.
    static bool readOne(const SensorDescriptor& sensor,
                        float& temperature, float& humidity);

    // Publie la paire température + humidité sur DataBus. Chemin de publication
    // commun aux deux origines : même validation META, même horodatage
    // VirtualClock, même écriture CSV, même publication MQTT.
    static void publishValues(const SensorDescriptor& sensor,
                              float temperature, float humidity);

    static bool readSensor(uint8_t address, float& temperature, float& humidity);
    static void drainRxBuffer();
    static uint16_t crc16(const uint8_t* data, size_t len);
};
