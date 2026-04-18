// src/Connectivity/MqttManager.h
#pragma once

#include <Arduino.h>
#include "Storage/DataLogger.h"

/*
 * MqttManager — Client MQTT non-bloquant (esp_mqtt natif ESP-IDF)
 *
 * Principe :
 * - esp_mqtt crée sa propre tâche FreeRTOS (TLS, reconnexion, callbacks)
 * - MqttManager expose un flag "mqttConnected" et un callback onDataPushed()
 * - Le TaskManager ne fait JAMAIS de réseau MQTT directement
 *
 * Intégration :
 * - init() appelé dans loopInit() après WiFiManager
 * - DataLogger::setOnPush(MqttManager::onDataPushed) pour publication auto
 * - Schéma JSON publié une seule fois au boot (retain=true sur serre/schema)
 * - LWT "offline" configuré dans esp_mqtt, "online" publié à la connexion
 *
 * Notification publication réussie :
 * - setOnPublishSuccess(callback) permet à un module externe (BridgeManager)
 *   d'être notifié à chaque publication MQTT réussie
 * - Le callback est appelé dans le contexte du TaskManager (pas de thread safety)
 * - Même pattern que DataLogger::setOnPush()
 *
 * Refactoring META :
 * - buildSchemaJson() lit tout depuis META (source de vérité unique)
 * - jsonEscape() et escapeCSV() centralisées dans DataLogger
 *
 * Dispatcher de commandes entrantes :
 * - Abonnement à "serre/cmd/#"
 * - Topics : serre/cmd/{id} où {id} est l'id META (ex: serre/cmd/4 = Valve1)
 * - Payload : durée en secondes en ASCII décimal
 * - Validation : isValidId + type == Actuator
 * - Délégation thread-safe via ValveManager::enqueueCommand() (queue FreeRTOS)
 */
class MqttManager {
public:
    static void init();
    static void onDataPushed(const DataRecord& record);
    static void ensureMqttStarted();
    static bool isMqttConnected();

    // ─── Callback publication réussie ────────────────────────────────────
    static void setOnPublishSuccess(void (*callback)());

private:
    static void* mqttClient;
    static volatile bool mqttConnected;
    static bool mqttStarted;
    static bool schemaPublished;

    static void mqttEventHandler(void* handlerArgs, const char* base, int32_t eventId, void* eventData);

    // Dispatcher des commandes entrantes (appelé sur MQTT_EVENT_DATA).
    // Parse serre/cmd/{id} + payload (secondes) et délègue à ValveManager
    // via sa queue FreeRTOS thread-safe.
    static void dispatchCommand(void* eventData);

    static void publishOnline();
    static void publishSchema();
    static String buildSchemaJson();

    static String formatCsvPayload(const DataRecord& record);

    // ─── Callback publication réussie ────────────────────────────────────
    static void (*_onPublishSuccess)();
};