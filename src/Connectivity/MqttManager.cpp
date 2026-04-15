// src/Connectivity/MqttManager.cpp
//
// Refactoring META (source de vérité unique) :
//  - Suppression ID_TO_TYPE[] (DataType vient de META)
//  - Suppression DATATYPE_LABELS[] (typeLabel vient de META)
//  - Suppression jsonEscape() locale (centralisée dans DataLogger)
//  - Suppression escapeCSV() locale (centralisée dans DataLogger)
//  - buildSchemaJson() génère tout depuis META

#include "Connectivity/MqttManager.h"
#include "Connectivity/WiFiManager.h"
#include "Config/NetworkConfig.h"
#include "Storage/DataLogger.h"
#include "Utils/Console.h"

#include "mqtt_client.h"
#include <variant>

// Tag pour logs Console
static const char* TAG = "MQTT";

// =============================================================================
// Variables statiques
// =============================================================================
void*         MqttManager::mqttClient      = nullptr;
volatile bool MqttManager::mqttConnected   = false;
bool          MqttManager::mqttStarted     = false;
bool          MqttManager::schemaPublished = false;

// Callback publication réussie (nullptr = pas de callback)
void (*MqttManager::_onPublishSuccess)() = nullptr;

// =============================================================================
// Callback publication réussie
// =============================================================================
void MqttManager::setOnPublishSuccess(void (*callback)())
{
    _onPublishSuccess = callback;
}

// =============================================================================
// Initialisation
// =============================================================================
void MqttManager::init()
{
    Console::info(TAG, "Initialisation client MQTT");

    esp_mqtt_client_config_t cfg = {};

    cfg.uri      = MQTT_BROKER_URI;
    cfg.cert_pem = MQTT_CA_CERT;

    cfg.username  = MQTT_USERNAME;
    cfg.password  = MQTT_PASSWORD;
    cfg.client_id = MQTT_CLIENT_ID;

    cfg.keepalive = MQTT_KEEPALIVE_S;

    cfg.lwt_topic  = MQTT_LWT_TOPIC;
    cfg.lwt_msg    = "offline";
    cfg.lwt_msg_len = 7;
    cfg.lwt_qos    = 1;
    cfg.lwt_retain = true;

    cfg.buffer_size = 1024;

    // Limite tout blocage interne d'esp_mqtt (mutex client, send TCP) a 2s
    // au lieu des 10s par defaut. Evite que esp_mqtt_client_publish bloque
    // la loop principale plus de 2s quand la socket TCP est bouchee (typiquement
    // pendant qu'un SMS coupe le PPP cote LilyGo).
    cfg.network_timeout_ms = 2000;

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
    if (!client) {
        Console::error(TAG, "Échec création client esp_mqtt");
        return;
    }

    mqttClient = client;

    esp_mqtt_client_register_event(
        client,
        (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
        mqttEventHandler,
        nullptr
    );

    mqttStarted = false;

    Console::info(TAG, "Client MQTT configuré (en attente WiFi STA)");
    Console::info(TAG, "Broker: " + String(MQTT_BROKER_URI));
    Console::info(TAG, "Client ID: " + String(MQTT_CLIENT_ID));
}

// =============================================================================
// Démarrage conditionnel
// =============================================================================
void MqttManager::ensureMqttStarted()
{
    if (mqttStarted || !mqttClient) return;
    if (!WiFiManager::isSTAConnected()) return;

    esp_err_t err = esp_mqtt_client_start((esp_mqtt_client_handle_t)mqttClient);
    if (err != ESP_OK) {
        Console::error(TAG, "Échec démarrage client esp_mqtt: " + String(err));
        return;
    }

    mqttStarted = true;
    Console::info(TAG, "Client MQTT démarré (WiFi STA disponible)");
}

// =============================================================================
// Event handler esp_mqtt
// =============================================================================
void MqttManager::mqttEventHandler(void* handlerArgs, const char* base,
                                    int32_t eventId, void* eventData)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)eventData;

    switch (eventId) {

    case MQTT_EVENT_CONNECTED:
        Console::info(TAG, "Connecté au broker");
        mqttConnected = true;

        publishOnline();

        if (!schemaPublished) {
            publishSchema();
            schemaPublished = true;
        }

        esp_mqtt_client_subscribe(
            (esp_mqtt_client_handle_t)mqttClient,
            "serre/cmd/#", 1
        );
        Console::info(TAG, "Abonné à serre/cmd/#");
        break;

    case MQTT_EVENT_DISCONNECTED:
        Console::warn(TAG, "Déconnecté du broker");
        mqttConnected = false;
        break;

    case MQTT_EVENT_ERROR:
        Console::error(TAG, "Erreur MQTT");
        if (event->error_handle &&
            event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            Console::error(TAG, "Erreur transport TCP/TLS");
        }
        break;

    case MQTT_EVENT_DATA:
        if (event->topic && event->topic_len > 0) {
            String topic(event->topic, event->topic_len);
            String payload(event->data, event->data_len);
            Console::info(TAG, "Message reçu: " + topic + " → " + payload);
            // TODO Phase 4 : dispatcher les commandes
        }
        break;

    default:
        break;
    }
}

// =============================================================================
// Publication "online" (retain)
// =============================================================================
void MqttManager::publishOnline()
{
    esp_mqtt_client_publish(
        (esp_mqtt_client_handle_t)mqttClient,
        MQTT_LWT_TOPIC,
        "online", 6,
        1, true
    );
    Console::info(TAG, "Publié 'online' sur " + String(MQTT_LWT_TOPIC));
}

// =============================================================================
// Publication du schéma (retain) — une seule fois au boot
// =============================================================================
void MqttManager::publishSchema()
{
    String json = buildSchemaJson();

    int msgId = esp_mqtt_client_publish(
        (esp_mqtt_client_handle_t)mqttClient,
        MQTT_SCHEMA_TOPIC,
        json.c_str(), json.length(),
        1, true
    );

    if (msgId >= 0) {
        Console::info(TAG, "Schéma publié sur " + String(MQTT_SCHEMA_TOPIC)
                     + " (" + String(json.length()) + " octets)");
    } else {
        Console::error(TAG, "Échec publication schéma");
    }
}

// =============================================================================
// Génération du schéma JSON depuis META (source de vérité unique)
// Format identique à buildBundleHeader() dans WebServer.cpp
// =============================================================================
String MqttManager::buildSchemaJson()
{
    String p;
    p.reserve(2048);

    p += "{\n";

    // Timestamp de génération (heure locale)
    char dateBuf[24] = "";
    {
        time_t now = time(nullptr);
        struct tm tmLocal;
        localtime_r(&now, &tmLocal);
        if (tmLocal.tm_year > 120) {
            strftime(dateBuf, sizeof(dateBuf), "%d-%m-%Y %H:%M:%S", &tmLocal);
        }
    }
    p += "  \"generated\": \""; p += dateBuf; p += "\",\n";

    // Colonnes CSV
    p += "  \"csvColumns\": [\"timestamp\", \"UTC_available\", \"UTC_reliable\", "
         "\"type\", \"id\", \"valueType\", \"value\"],\n";

    // ── Table DataType (dédupliquée depuis META) ────────────────────────────
    p += "  \"dataTypes\": [\n";
    bool firstType = true;
    for (uint8_t t = 0; t <= 3; t++) {
        const char* typeLabel = nullptr;
        for (size_t m = 0; m < META_COUNT; m++) {
            if ((uint8_t)META[m].type == t) {
                typeLabel = META[m].typeLabel;
                break;
            }
        }
        if (!typeLabel) continue;

        if (!firstType) p += ",\n";
        firstType = false;
        p += "    {\"id\": "; p += t;
        p += ", \"label\": \""; p += DataLogger::jsonEscape(typeLabel); p += "\"}";
    }
    p += "\n  ],\n";

    // ── Table DataId (depuis META) ──────────────────────────────────────────
    p += "  \"dataIds\": [\n";
    for (size_t i = 0; i < META_COUNT; i++) {
        const DataMeta& m = META[i];

        p += "    {\"id\": "; p += (uint8_t)m.id;
        p += ", \"label\": \""; p += DataLogger::jsonEscape(m.label); p += "\"";
        p += ", \"unit\": \"";  p += DataLogger::jsonEscape(m.unit);  p += "\"";

        const char* natureStr =
            (m.nature == DataNature::metrique) ? "metrique" :
            (m.nature == DataNature::etat)     ? "etat"     : "texte";
        p += ", \"nature\": \""; p += natureStr; p += "\"";

        p += ", \"type\": "; p += (uint8_t)m.type;

        // Min/Max (uniquement pour metrique)
        if (m.nature == DataNature::metrique) {
            p += ", \"min\": "; p += String(m.min, 1);
            p += ", \"max\": "; p += String(m.max, 1);
        }

        // Mapping états (uniquement pour nature == etat)
        if (m.nature == DataNature::etat && m.stateLabels != nullptr) {
            p += ", \"states\": [";
            for (uint8_t s = 0; s < m.stateLabelCount; s++) {
                if (s > 0) p += ", ";
                p += "{\"value\": "; p += s;
                p += ", \"label\": \"";
                if (m.stateLabels[s] != nullptr) {
                    p += DataLogger::jsonEscape(m.stateLabels[s]);
                }
                p += "\"}";
            }
            p += "]";
        }

        p += "}";
        if (i < META_COUNT - 1) p += ",";
        p += "\n";
    }
    p += "  ]\n";
    p += "}";

    return p;
}

// =============================================================================
// Callback DataLogger → publication MQTT
// =============================================================================
void MqttManager::onDataPushed(const DataRecord& record)
{
    ensureMqttStarted();
    if (!mqttConnected || !mqttClient) return;

    String topic = "serre/data/" + String((uint8_t)record.id);

    String csv = formatCsvPayload(record);

    int msgId = esp_mqtt_client_publish(
        (esp_mqtt_client_handle_t)mqttClient,
        topic.c_str(),
        csv.c_str(), csv.length(),
        0, false
    );

    if (msgId >= 0) {
        // Notification publication réussie (utilisé par BridgeManager pour le heartbeat)
        if (_onPublishSuccess) _onPublishSuccess();
    } else {
        Console::warn(TAG, "Échec publication " + topic);
    }
}

// =============================================================================
// Formatage CSV 7 champs
// timestamp,UTC_available,UTC_reliable,type,id,valueType,value
// =============================================================================
String MqttManager::formatCsvPayload(const DataRecord& record)
{
    String csv;
    csv.reserve(80);

    csv += String(record.timestamp);
    csv += ',';
    csv += String((int)record.UTC_available);
    csv += ',';
    csv += String((int)record.UTC_reliable);
    csv += ',';
    csv += String((uint8_t)record.type);
    csv += ',';
    csv += String((uint8_t)record.id);

    if (std::holds_alternative<float>(record.value)) {
        float val = std::get<float>(record.value);
        csv += ",0,";
        csv += String(val, 3);
    } else {
        String txt = std::get<String>(record.value);
        csv += ",1,";
        csv += DataLogger::escapeCSV(txt);
    }

    return csv;
}

// =============================================================================
// Accesseur
// =============================================================================
bool MqttManager::isMqttConnected()
{
    return mqttConnected;
}