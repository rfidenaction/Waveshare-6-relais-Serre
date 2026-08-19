// Sensors/SoilSensorRS485.h
// Lecture des sondes de sol ZTS-3000-TR-WS-N01 (Liyuan Electronic)
// via RS485 Modbus RTU sur Serial1 (UART1 isolé, direction auto hardware).
//
// Les capteurs sur le bus sont interrogés en rotation, un par appel de
// handle(). Chaque capteur fournit humidité sol (%) et température sol (°C).
//
// Architecture lecture / publication :
//   La lecture tourne vite — chaque sonde est lue à RS485_TEMP_READ_PERIOD_MS,
//   cadence plancher imposée par l'auto-échauffement de l'élément de mesure —
//   pour que l'arrosage conditionnel reste réactif. Chaque lecture est offerte
//   à ConditionalWatering.
//   La publication sur DataBus est découplée :
//     - à la première lecture réussie de chaque sonde
//     - toutes les heures (SOIL_RS485_PUBLISH_PERIOD_MS, battement par sonde)
//     - à la demande depuis l'UI (measureNow)
//     - immédiatement si la mesure déclenche un arrosage conditionnel
//       (publication faite par ConditionalWatering, pas par ce module)
#pragma once

#include <Arduino.h>
#include "Config/MetaDataModel.h"

class SoilSensorRS485 {
public:
    static void init();

    // Interroge UN capteur (rotation), offre ses mesures à l'arrosage
    // conditionnel, et publie sur DataBus si le battement de ce capteur est
    // échu.
    // Appelé périodiquement par TaskManager, à RS485_TEMP_READ_PERIOD_MS
    // divisé par sensorCount() : chaque sonde est ainsi lue à la cadence
    // plancher, quel que soit l'effectif.
    // Bloquant ~100 ms max (timeout Modbus).
    // Inactif si _maintenanceMode est true.
    static void handle();

    // Effectif de la famille, d'où main.cpp déduit la période de la tâche.
    // constexpr : la division est faite à la compilation.
    static constexpr uint8_t sensorCount() { return SENSOR_COUNT; }

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

    // ─── Mesure à la demande ─────────────────────────────────────────────
    // Ce module déclare les DataId qu'il produit ; il ne les reçoit d'aucune
    // table extérieure. La liste est dérivée de SENSORS[], seule source de
    // vérité de l'appartenance id ↔ adresse Modbus. OnDemandMeasure
    // l'interroge au démarrage pour construire sa vue id → propriétaire,
    // comme ValveManager se construit depuis RELAYS[].
    //
    // Conséquence : brancher les sondes 5 et 6 se limite à deux lignes dans
    // SENSORS[] et SENSOR_COUNT à 6. Le routage et la liste publiée sur MQTT
    // suivent au prochain démarrage, sans autre modification.

    // Nombre de DataId produits (2 par capteur : humidité + température).
    static uint8_t measurableCount();

    // DataId numéro `index`, avec index < measurableCount().
    static DataId measurableAt(uint8_t index);

    // Interroge immédiatement le capteur portant cet id et publie la paire
    // humidité + température sur DataBus — même chemin que handle(), donc
    // même validation, même horodatage, même journalisation CSV.
    // La rotation _currentSensor n'est pas touchée : le cycle périodique
    // suit son cours indépendamment.
    // Appelée depuis le thread TaskManager uniquement (bus RS485 partagé).
    // Retourne false si l'id est inconnu du module, si le bus est indisponible
    // (mode maintenance, délai de démarrage) ou si le capteur n'a pas répondu.
    static bool measureNow(DataId id);

private:
    static constexpr const char* TAG = "RS485";

    struct SensorDescriptor {
        uint8_t address;
        DataId  moistureId;
        DataId  temperatureId;
    };

    static constexpr uint8_t SENSOR_COUNT = 4;
    static_assert(SENSOR_COUNT > 0,
                  "SENSOR_COUNT divise RS485_TEMP_READ_PERIOD_MS dans main.cpp");

    static constexpr SensorDescriptor SENSORS[SENSOR_COUNT] = {
        { 0x01, DataId::SoilMoisture1, DataId::SoilTemperature1 },
        { 0x02, DataId::SoilMoisture2, DataId::SoilTemperature2 },
        { 0x03, DataId::SoilMoisture3, DataId::SoilTemperature3 },
        { 0x04, DataId::SoilMoisture4, DataId::SoilTemperature4 },
    };

    static bool    _initialized;
    static bool    _maintenanceMode;
    static uint8_t _currentSensor;

    // Battement de publication, propre à chaque sonde et posé à sa première
    // lecture réussie, jamais recalé ensuite. La rotation étale donc les
    // publications des sondes dans l'heure sans qu'aucun décalage soit calculé.
    static unsigned long _lastPublishMs[SENSOR_COUNT];
    static bool          _firstPublishDone[SENSOR_COUNT];

    // Interroge le capteur décrit par `sensor` : transaction Modbus et contrôle
    // de vraisemblance, sans rien publier. Chemin de lecture commun à
    // l'acquisition périodique et à la mesure à la demande — un seul endroit
    // décide de ce qui est rejeté.
    static bool readOne(const SensorDescriptor& sensor,
                        float& moisture, float& temperature);

    // Publie la paire humidité + température sur DataBus. Chemin de publication
    // commun aux deux origines : même validation META, même horodatage
    // VirtualClock, même écriture CSV, même publication MQTT.
    static void publishValues(const SensorDescriptor& sensor,
                              float moisture, float temperature);

    static bool readSensor(uint8_t address, float& moisture, float& temperature);
    static void drainRxBuffer();
    static uint16_t crc16(const uint8_t* data, size_t len);
};