// src/Connectivity/MqttManager.cpp
//
// Client MQTT non-bloquant. Voir MqttManager.h pour l'architecture générale
// (tampon FIFO amont + watchdog zombie).
//
// Dépendance à META : tous les IDs, types et libellés manipulés ici
// proviennent de META (tableau DataLogger). META est la référence UNIQUE
// du projet pour la sémantique des datas. Aucune table locale ne duplique
// ni ne redéfinit quoi que ce soit de META.

#include "Connectivity/MqttManager.h"
#include "Connectivity/WiFiManager.h"
#include "Config/NetworkConfig.h"
#include "Storage/DataLogger.h"
#include "Actuators/ValveManager.h"
#include "Utils/Console.h"

#include "mqtt_client.h"
#include <variant>
#include <stdlib.h>
#include <string.h>

static const char* TAG = "MQTT";

// ─── Variables statiques ─────────────────────────────────────────────────
void*         MqttManager::mqttClient      = nullptr;
volatile bool MqttManager::mqttConnected   = false;
bool          MqttManager::mqttStarted     = false;
bool          MqttManager::schemaPublished = false;

void (*MqttManager::_onPublishSuccess)() = nullptr;

MqttManager::BufferSlot MqttManager::buffer[BUFFER_CAPACITY] = {};
uint8_t  MqttManager::bufferHead    = 0;
uint8_t  MqttManager::bufferCount   = 0;
uint32_t MqttManager::evictionCount = 0;

volatile uint32_t MqttManager::messagesEnqueued  = 0;
volatile uint32_t MqttManager::messagesPublished = 0;
volatile uint32_t MqttManager::watchdogSeconds   = MqttManager::WATCHDOG_SECONDS;
uint32_t          MqttManager::forcedDisconnectCount = 0;

// =============================================================================
void MqttManager::setOnPublishSuccess(void (*callback)())
{
    _onPublishSuccess = callback;
}

// =============================================================================
// Initialisation (esp_mqtt configuré mais pas démarré : attend WiFi STA).
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

    // Filet de sécurité pour le thread esp_mqtt (2 s au lieu des 10 s par défaut).
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
    Console::info(TAG, "Tampon amont : " + String(BUFFER_CAPACITY) + " entrées");
    Console::info(TAG, "Watchdog zombie : seuil gap=" + String(WATCHDOG_GAP_THRESHOLD)
                      + ", fenêtre=" + String(WATCHDOG_SECONDS / 60) + " min");
}

// =============================================================================
// Démarrage effectif (esp_mqtt_client_start) dès que WiFi STA est up.
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
// Event handler esp_mqtt — tourne dans le thread esp_mqtt (PAS TaskManager).
// Toute action déclenchée ici doit être non-bloquante et thread-safe.
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

    case MQTT_EVENT_PUBLISHED:
        // PUBACK reçu (QoS 1). Reset du watchdog zombie + notification externe.
        messagesEnqueued  = 0;
        messagesPublished = 0;
        watchdogSeconds   = WATCHDOG_SECONDS;

        if (_onPublishSuccess) _onPublishSuccess();
        break;

    case MQTT_EVENT_ERROR:
        Console::error(TAG, "Erreur MQTT");
        if (event->error_handle &&
            event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            Console::error(TAG, "Erreur transport TCP/TLS");
        }
        break;

    case MQTT_EVENT_DATA:
        dispatchCommand(event);
        break;

    default:
        break;
    }
}

// =============================================================================
// Dispatcher des commandes entrantes serre/cmd/{id} (payload = durée en s).
// Validation via META : seuls les DataId déclarés dans META avec
// type == Actuator sont acceptés (isValidId puis getMeta().type).
// Délègue à ValveManager via sa queue FreeRTOS (aucun accès direct depuis
// le thread esp_mqtt aux structures non thread-safe).
// =============================================================================
void MqttManager::dispatchCommand(void* eventData)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)eventData;

    if (!event->topic || event->topic_len <= 0) return;

    // Les champs topic/data d'esp_mqtt ne sont pas null-terminés.
    char topicBuf[64];
    char dataBuf[32];

    int tlen = event->topic_len;
    if (tlen >= (int)sizeof(topicBuf)) tlen = sizeof(topicBuf) - 1;
    memcpy(topicBuf, event->topic, tlen);
    topicBuf[tlen] = '\0';

    int dlen = event->data_len;
    if (dlen < 0) dlen = 0;
    if (dlen >= (int)sizeof(dataBuf)) dlen = sizeof(dataBuf) - 1;
    if (dlen > 0) memcpy(dataBuf, event->data, dlen);
    dataBuf[dlen] = '\0';

    static const char PREFIX[] = "serre/cmd/";
    const int PREFIX_LEN = sizeof(PREFIX) - 1;
    if (strncmp(topicBuf, PREFIX, PREFIX_LEN) != 0) {
        Console::warn(TAG, "Topic commande inattendu : " + String(topicBuf));
        return;
    }

    const char* idStr = topicBuf + PREFIX_LEN;
    if (*idStr == '\0') {
        Console::warn(TAG, "Topic commande sans id : " + String(topicBuf));
        return;
    }

    char* endPtr = nullptr;
    long idLong = strtol(idStr, &endPtr, 10);
    if (endPtr == idStr || *endPtr != '\0' || idLong < 0 || idLong > 255) {
        Console::warn(TAG, "Id non numérique dans topic : " + String(topicBuf));
        return;
    }
    uint8_t idByte = (uint8_t)idLong;

    if (!DataLogger::isValidId(idByte)) {
        Console::warn(TAG, "Id inconnu de META : " + String(idByte));
        return;
    }

    DataId id = (DataId)idByte;
    const DataMeta& meta = DataLogger::getMeta(id);
    if (meta.type != DataType::Actuator) {
        Console::warn(TAG, "Commande sur DataId non-actionneur : id=" +
                      String(idByte) + " (" + String(meta.label) + ")");
        return;
    }

    if (dlen == 0) {
        Console::warn(TAG, "Payload vide pour commande id=" + String(idByte));
        return;
    }

    endPtr = nullptr;
    long secLong = strtol(dataBuf, &endPtr, 10);
    if (endPtr == dataBuf || secLong <= 0) {
        Console::warn(TAG, "Durée invalide pour commande id=" + String(idByte) +
                      " : '" + String(dataBuf) + "'");
        return;
    }

    // Trace d'intention : enregistrée dès validation, AVANT ValveManager.
    // Non-bloquant — passe par la queue intake thread-safe de DataLogger.
    DataId cmdId;
    if (ValveManager::commandIdForValve(id, cmdId)) {
        DataLogger::enqueueCommand(cmdId, DataType::CommandManual, (float)secLong);
    } else {
        Console::warn(TAG, "Pas de commande associée pour id=" + String(idByte) +
                      " (slots pas encore construits ?) — log commande omis");
    }

    ValveManager::ValveCommand cmd;
    cmd.id         = id;
    cmd.durationMs = (uint32_t)secLong * 1000UL;

    if (ValveManager::enqueueCommand(cmd)) {
        Console::info(TAG, "Commande acceptée : id=" + String(idByte) +
                      " (" + String(meta.label) + ") pour " +
                      String(secLong) + " s");
    } else {
        Console::warn(TAG, "Commande rejetée (queue pleine) : id=" +
                      String(idByte));
    }
}

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
// Publication du schéma JSON (retain) — une seule fois au boot.
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
// Génération du schéma JSON depuis META (source de vérité unique).
// Format identique à buildBundleHeader() dans WebServer.cpp.
// =============================================================================
String MqttManager::buildSchemaJson()
{
    String p;
    p.reserve(2048);

    p += "{\n";

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

    p += "  \"csvColumns\": [\"timestamp\", \"UTC_available\", \"UTC_reliable\", "
         "\"type\", \"id\", \"valueType\", \"value\"],\n";

    // Table DataType — énumère TOUS les types possibles (META + records),
    // y compris ceux absents de META (CommandManual, CommandAuto). Libellés
    // fournis par DataLogger::typeLabel (source de vérité unique).
    p += "  \"dataTypes\": [\n";
    bool firstType = true;
    for (uint8_t t = 0; t <= (uint8_t)DataType::CommandAuto; t++) {
        if (!firstType) p += ",\n";
        firstType = false;
        p += "    {\"id\": "; p += t;
        p += ", \"label\": \"";
        p += DataLogger::jsonEscape(DataLogger::typeLabel((DataType)t));
        p += "\"}";
    }
    p += "\n  ],\n";

    // Table DataId (depuis META).
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

        if (m.nature == DataNature::metrique) {
            p += ", \"min\": "; p += String(m.min, 1);
            p += ", \"max\": "; p += String(m.max, 1);
        }

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
// Callback DataLogger → alimentation du tampon FIFO amont.
// Alimente TOUJOURS, même hors connexion. Éviction de la plus ancienne si plein.
// =============================================================================
void MqttManager::onDataPushed(const DataRecord& record)
{
    String csv = formatCsvPayload(record);

    if (csv.length() >= sizeof(buffer[0].payload)) {
        Console::warn(TAG, "Payload trop longue (" + String(csv.length())
                          + " octets) pour id=" + String((uint8_t)record.id)
                          + " — message non publié sur MQTT");
        return;
    }

    if (bufferCount == BUFFER_CAPACITY) {
        evictionCount++;
        Console::warn(TAG, "Éviction tampon : message id="
                          + String(buffer[bufferHead].id)
                          + " perdu pour MQTT (cumul évictions : "
                          + String(evictionCount) + ")");
        bufferHead = (bufferHead + 1) % BUFFER_CAPACITY;
        bufferCount--;
    }

    uint8_t writeIdx = (bufferHead + bufferCount) % BUFFER_CAPACITY;
    buffer[writeIdx].id = (uint8_t)record.id;
    strncpy(buffer[writeIdx].payload, csv.c_str(),
            sizeof(buffer[writeIdx].payload) - 1);
    buffer[writeIdx].payload[sizeof(buffer[writeIdx].payload) - 1] = '\0';
    bufferCount++;
}

// =============================================================================
// Drain 1 entrée/s du tampon vers esp-mqtt + watchdog zombie.
// Tâche TaskManager période 1 s. Non-bloquant (latence max 2 s via
// network_timeout_ms). En cas d'échec enqueue, l'entrée reste en tête et
// sera retentée au prochain tour.
// =============================================================================
void MqttManager::handle()
{
    if (!mqttClient) return;
    ensureMqttStarted();
    if (!mqttConnected) return;

    if (watchdogSeconds > 0) {
        watchdogSeconds--;
    }

    if (bufferCount > 0) {
        const BufferSlot& slot = buffer[bufferHead];
        String topic = "serre/data/" + String(slot.id);

        int msgId = esp_mqtt_client_enqueue(
            (esp_mqtt_client_handle_t)mqttClient,
            topic.c_str(),
            slot.payload, strlen(slot.payload),
            1,           // qos=1 (requis pour MQTT_EVENT_PUBLISHED)
            false,       // retain=false
            true         // store=true (requis pour que enqueue accepte)
        );

        if (msgId >= 0) {
            messagesEnqueued++;
            bufferHead = (bufferHead + 1) % BUFFER_CAPACITY;
            bufferCount--;
        } else {
            Console::warn(TAG, "Enqueue échoué id=" + String(slot.id)
                              + " — réessai au prochain tour");
        }
    }

    // Cast int32_t : tolère une race PUBACK vs enqueue sans fausse alerte.
    int32_t gap = (int32_t)(messagesEnqueued - messagesPublished);
    if (gap >= (int32_t)WATCHDOG_GAP_THRESHOLD && watchdogSeconds == 0) {
        forcedDisconnectCount++;
        Console::warn(TAG,
            "Zombie MQTT détecté (gap=" + String(gap) +
            ", " + String(WATCHDOG_SECONDS / 60) + " min sans PUBACK) — "
            "disconnect forcé (cumul depuis boot : " +
            String(forcedDisconnectCount) + ")");

        esp_mqtt_client_disconnect((esp_mqtt_client_handle_t)mqttClient);

        messagesEnqueued  = 0;
        messagesPublished = 0;
        watchdogSeconds   = WATCHDOG_SECONDS;
    }
}

// =============================================================================
// Formatage CSV 7 champs :
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
bool MqttManager::isMqttConnected()
{
    return mqttConnected;
}
