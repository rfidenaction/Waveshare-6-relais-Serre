// src/Connectivity/MqttManager.cpp

#include "Connectivity/MqttManager.h"
#include "Connectivity/WiFiManager.h"
#include "Config/NetworkConfig.h"
#include "Storage/DataLogger.h"
#include "Utils/Console.h"

#include "mqtt_client.h"   // esp_mqtt natif ESP-IDF (disponible via Arduino ESP32 core)
#include <variant>

// Tag pour logs Console
static const char* TAG = "MQTT";

// =============================================================================
// Mapping DataId → DataType (pour le schéma JSON)
// Même approche que BUNDLE_ID_TO_TYPE dans WebServer.cpp.
// CONTRAT : synchronisé avec l'enum DataId dans DataLogger.h.
// =============================================================================
static const uint8_t ID_TO_TYPE[(uint8_t)DataId::Count] = {
    0,  // SupplyVoltage    (0)  → Power
    1,  // AirTemperature1  (1)  → Sensor
    1,  // AirHumidity1     (2)  → Sensor
    1,  // SoilMoisture1    (3)  → Sensor
    2,  // Valve1           (4)  → Actuator
    3,  // WifiStaConnected (5)  → System
    3,  // WifiApEnabled    (6)  → System
    3,  // WifiRssi         (7)  → System
    3,  // Boot             (8)  → System
    3,  // Error            (9)  → System
};

// Labels DataType en français (pour le schéma JSON)
static const char* DATATYPE_LABELS[] = {
    "Alimentation", "Capteurs", "Actionneurs", "Système"
};

// =============================================================================
// Variables statiques
// =============================================================================
void*         MqttManager::mqttClient      = nullptr;
volatile bool MqttManager::mqttConnected   = false;
bool          MqttManager::mqttStarted     = false;
bool          MqttManager::schemaPublished = false;

// =============================================================================
// Initialisation
// =============================================================================
void MqttManager::init()
{
    Console::info(TAG, "Initialisation client MQTT");

    // API ESP-IDF 4.x : champs plats (pas de struct imbriquée)
    esp_mqtt_client_config_t cfg = {};

    // Broker + TLS
    cfg.uri      = MQTT_BROKER_URI;
    cfg.cert_pem = MQTT_CA_CERT;

    // Credentials
    cfg.username  = MQTT_USERNAME;
    cfg.password  = MQTT_PASSWORD;
    cfg.client_id = MQTT_CLIENT_ID;

    // Session
    cfg.keepalive = MQTT_KEEPALIVE_S;

    // LWT (Last Will and Testament)
    cfg.lwt_topic  = MQTT_LWT_TOPIC;
    cfg.lwt_msg    = "offline";
    cfg.lwt_msg_len = 7;
    cfg.lwt_qos    = 1;
    cfg.lwt_retain = true;

    // Taille buffer (défaut 1024, suffisant pour nos payloads CSV)
    cfg.buffer_size = 1024;

    // Création du client
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
    if (!client) {
        Console::error(TAG, "Échec création client esp_mqtt");
        return;
    }

    mqttClient = client;

    // Enregistrement du handler d'événements
    // Signature ESP-IDF 4.x : void (*)(void*, const char*, int32_t, void*)
    esp_mqtt_client_register_event(
        client,
        (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
        mqttEventHandler,
        nullptr
    );

    // Le client est prêt mais PAS démarré.
    // ensureMqttStarted() le démarrera dès que le WiFi STA sera connecté.
    mqttStarted = false;

    Console::info(TAG, "Client MQTT configuré (en attente WiFi STA)");
    Console::info(TAG, "Broker: " + String(MQTT_BROKER_URI));
    Console::info(TAG, "Client ID: " + String(MQTT_CLIENT_ID));
}

// =============================================================================
// Démarrage conditionnel — appelé depuis la tâche WiFi status
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
// S'exécute dans la tâche FreeRTOS de esp_mqtt, PAS dans TaskManager.
// Ne touche que le flag "mqttConnected" et les publications initiales.
//
// Signature ESP-IDF 4.x : (void* handler_args, esp_event_base_t base,
//                           int32_t event_id, void* event_data)
// =============================================================================
void MqttManager::mqttEventHandler(void* handlerArgs, const char* base,
                                    int32_t eventId, void* eventData)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)eventData;

    switch (eventId) {

    case MQTT_EVENT_CONNECTED:
        Console::info(TAG, "Connecté au broker");
        mqttConnected = true;

        // Publier "online" (retain)
        publishOnline();

        // Publier le schéma une seule fois par session
        if (!schemaPublished) {
            publishSchema();
            schemaPublished = true;
        }

        // S'abonner aux commandes (préparation Phase 4)
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
        // Réception message (futur : commandes serre/cmd/#)
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
        1,      // QoS 1
        true    // retain
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
        1,      // QoS 1
        true    // retain
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

    // Table DataType
    p += "  \"dataTypes\": [\n";
    for (int i = 0; i < 4; i++) {
        p += "    {\"id\": "; p += i;
        p += ", \"label\": \""; p += DATATYPE_LABELS[i]; p += "\"}";
        if (i < 3) p += ",";
        p += "\n";
    }
    p += "  ],\n";

    // Table DataId (depuis META)
    p += "  \"dataIds\": [\n";
    const uint8_t count = (uint8_t)DataId::Count;
    for (uint8_t i = 0; i < count; i++) {
        const DataMeta& m = DataLogger::getMeta((DataId)i);

        p += "    {\"id\": "; p += i;
        p += ", \"label\": \""; p += jsonEscape(m.label); p += "\"";
        p += ", \"unit\": \"";  p += jsonEscape(m.unit);  p += "\"";

        const char* natureStr =
            (m.nature == DataNature::metrique) ? "metrique" :
            (m.nature == DataNature::etat)     ? "etat"     : "texte";
        p += ", \"nature\": \""; p += natureStr; p += "\"";

        p += ", \"type\": "; p += ID_TO_TYPE[i];

        // Mapping états (uniquement pour nature == etat)
        if (m.nature == DataNature::etat && m.stateLabels != nullptr) {
            p += ", \"states\": [";
            for (uint8_t s = 0; s < m.stateLabelCount; s++) {
                if (s > 0) p += ", ";
                p += "{\"value\": "; p += s;
                p += ", \"label\": \"";
                if (m.stateLabels[s] != nullptr) {
                    p += jsonEscape(m.stateLabels[s]);
                }
                p += "\"}";
            }
            p += "]";
        }

        p += "}";
        if (i < count - 1) p += ",";
        p += "\n";
    }
    p += "  ]\n";
    p += "}";

    return p;
}

// =============================================================================
// Callback DataLogger → publication MQTT
// Appelé depuis push() dans le contexte TaskManager.
// esp_mqtt_client_publish() est thread-safe (met en file interne).
// =============================================================================
void MqttManager::onDataPushed(const DataRecord& record)
{
    ensureMqttStarted();
    if (!mqttConnected || !mqttClient) return;

    // Topic : serre/data/{id}
    String topic = "serre/data/" + String((uint8_t)record.id);

    // Payload CSV 7 champs
    String csv = formatCsvPayload(record);

    int msgId = esp_mqtt_client_publish(
        (esp_mqtt_client_handle_t)mqttClient,
        topic.c_str(),
        csv.c_str(), csv.length(),
        0,      // QoS 0 (données capteurs)
        false   // pas de retain
    );

    if (msgId < 0) {
        Console::warn(TAG, "Échec publication " + topic);
    }
}

// =============================================================================
// Formatage CSV 7 champs (identique à flushToFlash dans DataLogger.cpp)
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
        csv += escapeCSV(txt);
    }

    return csv;
}

// =============================================================================
// Échappement CSV (guillemets, doublage guillemets internes)
// Même logique que escapeCSV() dans DataLogger.cpp
// =============================================================================
String MqttManager::escapeCSV(const String& text)
{
    String escaped = "\"";
    for (size_t i = 0; i < text.length(); i++) {
        char c = text.charAt(i);
        if (c == '"') {
            escaped += "\"\"";
        } else {
            escaped += c;
        }
    }
    escaped += "\"";
    return escaped;
}

// =============================================================================
// Échappement JSON minimal
// Même logique que jsonEscape() dans WebServer.cpp
// =============================================================================
String MqttManager::jsonEscape(const char* s)
{
    String out;
    if (!s) return out;
    out.reserve(strlen(s) + 4);
    while (*s) {
        char c = *s++;
        if      (c == '"')  { out += '\\'; out += '"';  }
        else if (c == '\\') { out += '\\'; out += '\\'; }
        else if (c == '\n') { out += '\\'; out += 'n';  }
        else if (c == '\r') { out += '\\'; out += 'r';  }
        else                { out += c; }
    }
    return out;
}

// =============================================================================
// Accesseur
// =============================================================================
bool MqttManager::isMqttConnected()
{
    return mqttConnected;
}