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
 * Refactoring META :
 * - buildSchemaJson() lit tout depuis META (source de vérité unique)
 * - jsonEscape() et escapeCSV() centralisées dans DataLogger
 */
class MqttManager {
public:
    static void init();
    static void onDataPushed(const DataRecord& record);
    static void ensureMqttStarted();
    static bool isMqttConnected();

private:
    static void* mqttClient;
    static volatile bool mqttConnected;
    static bool mqttStarted;
    static bool schemaPublished;

    static void mqttEventHandler(void* handlerArgs, const char* base, int32_t eventId, void* eventData);

    static void publishOnline();
    static void publishSchema();
    static String buildSchemaJson();

    static String formatCsvPayload(const DataRecord& record);
};