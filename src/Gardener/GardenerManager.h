// src/Gardener/GardenerManager.h
//
// Programmateur d'arrosage automatique.
//
// Gère jusqu'à 6 créneaux par vanne (36 au total). Chaque créneau définit
// une heure de début (heure locale) et une durée en secondes. Les créneaux
// sont identiques tous les jours, persistés en LittleFS, et pilotés via
// les topics MQTT serre/gardener/FromUser et serre/gardener/ToUser.
//
// Au déclenchement d'un créneau, GardenerManager construit un BusItem
// (DataType::CommandAuto) et le publie via DataBus::publish(), ce qui
// enclenche la chaîne existante (validation META, horodatage, distribution
// mqttQueue/logQueue/WebServer, routage via RELAYS[] → ValveManager).
//
// Intégration :
//   - init() appelé dans loopInit() après MqttManager::init()
//   - handle() en tâche TaskManager période 1000 ms
//   - onGardenerMessage() appelé par MqttManager depuis le thread esp_mqtt
//   - publishGardenerWateringState() appelé par MqttManager sur MQTT_EVENT_CONNECTED
#pragma once

#include <Arduino.h>
#include "Config/MetaDataModel.h"

// ═════════════════════════════════════════════════════════════════════════════
// GardenerWateringSlot — un créneau d'arrosage programmé
// ═════════════════════════════════════════════════════════════════════════════

struct GardenerWateringSlot {
    DataId   cmdId;               // CommandValve1..CommandValve6 (ids 17..22)
    uint8_t  hour;                // 0..23 (heure locale)
    uint8_t  minute;              // 0..59
    uint16_t duration;            // secondes, 1..900
    bool     cancellableBySensor; // toujours false en v1, ignoré au déclenchement
};

// ═════════════════════════════════════════════════════════════════════════════
// GardenerManager — API statique
// ═════════════════════════════════════════════════════════════════════════════

class GardenerManager {
public:
    // Topics MQTT (publics pour MqttManager)
    static constexpr const char* GARDENER_TOPIC_FROM_USER = "serre/gardener/FromUser";
    static constexpr const char* GARDENER_TOPIC_TO_USER   = "serre/gardener/ToUser";

    // Charge /gardener.json. Appeler une fois au boot, après LittleFS.begin().
    static void init();

    // Tâche périodique 1000 ms. Traite les messages MQTT bufferisés et
    // déclenche les arrosages programmés à chaque transition de minute.
    static void handle();

    // Réception d'un message MQTT sur serre/gardener/FromUser.
    // Appelée depuis le thread esp_mqtt — bufferise le message brut pour
    // traitement dans handle() (thread TaskManager).
    static void onGardenerMessage(const char* data, int len);

    // Sérialise l'état courant et publie via MqttManager (retain).
    // Appelée depuis handle() après chaque add/remove, et depuis
    // MqttManager::mqttEventHandler sur MQTT_EVENT_CONNECTED.
    static void publishGardenerWateringState();

private:
    static constexpr uint8_t MAX_WATERING_SLOTS_PER_VALVE = 6;
    static constexpr uint8_t MAX_WATERING_SLOTS_TOTAL     = 36;  // 6 × 6

    static GardenerWateringSlot gardenerWateringSlots[];
    static uint8_t  gardenerWateringSlotCount;
    static uint16_t gardenerLastMinute;

    // Buffer MQTT (thread esp_mqtt → thread TaskManager).
    // Un seul writer (onGardenerMessage), un seul reader (handle).
    // Si un nouveau message arrive avant traitement, il écrase le précédent.
    static constexpr size_t MSG_BUFFER_SIZE = 256;
    static char          gardenerMsgBuffer[];
    static volatile bool gardenerMsgPending;

    // Traitement du message bufferisé (appelé depuis handle).
    static void processGardenerMessage();

    // Ajout/suppression avec validation complète + sauvegarde.
    static bool addGardenerWateringSlot(const GardenerWateringSlot& slot);
    static bool removeGardenerWateringSlot(DataId cmdId, uint8_t hour, uint8_t minute);

    // Validation des champs d'un slot (bornes, cmdId valide).
    static bool validateGardenerWateringSlot(const GardenerWateringSlot& slot);

    // Chevauchement temporel sur la même vanne (modulo 1440 minutes).
    static bool hasGardenerTimeOverlap(const GardenerWateringSlot& slot);

    // Nombre de créneaux existants pour une vanne donnée.
    static uint8_t countGardenerSlotsForValve(DataId cmdId);

    // Persistance LittleFS (écriture atomique via .tmp + rename).
    static bool saveGardenerWateringSlots();
    static bool loadGardenerWateringSlots();

    // Sérialisation JSON (format commun persistance + MQTT ToUser).
    static String serializeGardenerWateringSlots();
};