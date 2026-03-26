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
 */
class MqttManager {
public:
    // -------------------------------------------------------------------------
    // Initialisation
    // -------------------------------------------------------------------------
    
    /**
     * Initialise le client esp_mqtt avec TLS et LWT.
     * Crée sa propre tâche FreeRTOS (non-bloquant).
     * Doit être appelé APRÈS WiFiManager::init().
     */
    static void init();

    // -------------------------------------------------------------------------
    // Callback pour DataLogger
    // -------------------------------------------------------------------------
    
    /**
     * Appelé par DataLogger::push() à chaque nouvelle donnée.
     * Publie immédiatement sur serre/data/{id} si connecté.
     * Non-bloquant : esp_mqtt_client_publish() met en file interne.
     */
    static void onDataPushed(const DataRecord& record);

    // -------------------------------------------------------------------------
    // Démarrage conditionnel (appelé depuis la tâche WiFi status)
    // -------------------------------------------------------------------------
    
    /**
     * Démarre esp_mqtt si le WiFi STA est connecté et que le client
     * n'a pas encore été démarré. Sans effet si déjà démarré.
     * Non-bloquant : esp_mqtt_client_start() crée la tâche FreeRTOS interne.
     */
    static void ensureMqttStarted();

    // -------------------------------------------------------------------------
    // État (lecture seule)
    // -------------------------------------------------------------------------
    static bool isMqttConnected();

private:
    // -------------------------------------------------------------------------
    // Client esp_mqtt
    // -------------------------------------------------------------------------
    static void* mqttClient;           // esp_mqtt_client_handle_t (void* pour éviter include)
    static volatile bool mqttConnected; // Flag posé par le callback esp_mqtt
    static bool mqttStarted;            // esp_mqtt_client_start() déjà appelé
    static bool schemaPublished;        // Schéma publié dans cette session

    // -------------------------------------------------------------------------
    // Event handler esp_mqtt (s'exécute dans la tâche esp_mqtt, PAS TaskManager)
    // Signature ESP-IDF 4.x : (void*, const char*, int32_t, void*)
    // -------------------------------------------------------------------------
    static void mqttEventHandler(void* handlerArgs, const char* base, int32_t eventId, void* eventData);

    // -------------------------------------------------------------------------
    // Publication
    // -------------------------------------------------------------------------
    static void publishOnline();        // "online" retain sur serre/status/waveshare
    static void publishSchema();        // Schéma JSON retain sur serre/schema
    static String buildSchemaJson();    // Génération JSON depuis META

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------
    static String formatCsvPayload(const DataRecord& record);
    static String escapeCSV(const String& text);
    static String jsonEscape(const char* s);
};