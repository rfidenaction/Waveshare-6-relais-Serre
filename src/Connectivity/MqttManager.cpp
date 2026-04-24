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
#include "Connectivity/BridgeManager.h"
#include "Connectivity/WiFiManager.h"
#include "Config/NetworkConfig.h"
#include "Storage/DataLogger.h"
#include "Core/CommandRouter.h"
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

char    MqttManager::inFlightPayload[200] = {};
uint8_t MqttManager::inFlightId           = 0;
bool    MqttManager::inFlightBusy         = false;

volatile uint32_t MqttManager::messagesEnqueued  = 0;
volatile uint32_t MqttManager::messagesPublished = 0;
// Sémantique révisée : watchdogSeconds porte désormais l'horodatage millis()
// du dernier PUBACK reçu (ou du dernier reset post-disconnect). Le nom est
// conservé pour limiter le périmètre du patch ; la fenêtre de 65 min est
// évaluée par comparaison (millis() - watchdogSeconds) >= WATCHDOG_SECONDS*1000,
// ce qui la rend insensible à la période du tick handle().
// Valeur initiale = WATCHDOG_SECONDS : la première alerte possible reste à
// ~65 min d'uptime, strictement comme le comportement d'origine.
volatile uint32_t MqttManager::watchdogSeconds   = MqttManager::WATCHDOG_SECONDS;
uint32_t          MqttManager::forcedDisconnectCount = 0;

uint32_t MqttManager::mqttKoDownSinceMs = 0;
uint32_t MqttManager::mqttKoLastSentMs  = 0;
uint32_t MqttManager::mqttKoSentCount   = 0;

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
    Console::info(TAG, "Slot in-flight : 1 record (backpressure via DataLogger::logBufferOut)");
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

        // Desarme la temporisation MqttKo : le broker est de nouveau joignable.
        mqttKoDownSinceMs = 0;
        mqttKoLastSentMs  = 0;

        publishOnline();

        if (!schemaPublished) {
            publishSchema();
            schemaPublished = true;
        }

        esp_mqtt_client_subscribe(
            (esp_mqtt_client_handle_t)mqttClient,
            "serre/cmd", 1
        );
        Console::info(TAG, "Abonné à serre/cmd");
        break;

    case MQTT_EVENT_DISCONNECTED:
        Console::warn(TAG, "Déconnecté du broker");
        mqttConnected = false;

        // Arme la temporisation MqttKo au premier evenement DISCONNECTED
        // de l'episode courant. Si deja armee, on conserve le t0 d'origine
        // (les MQTT_EVENT_DISCONNECTED repetes ne reinitialisent pas l'horloge).
        if (mqttKoDownSinceMs == 0) {
            mqttKoDownSinceMs = millis();
            mqttKoLastSentMs  = 0;
        }
        break;

    case MQTT_EVENT_PUBLISHED:
        // PUBACK reçu (QoS 1). Reset du watchdog zombie + notification externe.
        messagesEnqueued  = 0;
        messagesPublished = 0;
        watchdogSeconds   = millis();   // horodatage du dernier PUBACK (ms)

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
// Dispatcher des commandes entrantes sur serre/cmd. Payload = CSV 7 champs
// "timestamp,VClock_available,VClock_reliable,type,id,valueType,value" dont
// les 3 premiers doivent être vides ou "0" (l'émetteur n'horodate pas).
//
// Ce module ne connaît aucun actionneur par son nom ; il vérifie le topic puis
// orchestre trois étapes aux responsabilités disjointes :
//   1. DataLogger::parseCommand    : décode et valide (fonction pure).
//   2. DataLogger::traceCommand    : journalise dans PENDING (best-effort).
//   3. CommandRouter::route        : exécute via RELAYS[].
// Aucun return de MQTT vers l'émetteur : le protocole est fire-and-forget.
// Les rejets sont simplement loggés.
// =============================================================================
void MqttManager::dispatchCommand(void* eventData)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)eventData;

    if (!event->topic || event->topic_len <= 0) return;

    // Vérification topic strict (abonnement = match exact "serre/cmd").
    static const char TOPIC_CMD[] = "serre/cmd";
    const int TOPIC_LEN = sizeof(TOPIC_CMD) - 1;
    if (event->topic_len != TOPIC_LEN ||
        memcmp(event->topic, TOPIC_CMD, TOPIC_LEN) != 0) {
        char topicBuf[64];
        int tlen = event->topic_len;
        if (tlen >= (int)sizeof(topicBuf)) tlen = sizeof(topicBuf) - 1;
        memcpy(topicBuf, event->topic, tlen);
        topicBuf[tlen] = '\0';
        Console::warn(TAG, "Topic inattendu : " + String(topicBuf));
        return;
    }

    DataLogger::ParsedCommand cmd;
    auto res = DataLogger::parseCommand(
        event->data, (size_t)event->data_len, cmd);
    if (res != DataLogger::CommandParseResult::OK) {
        Console::warn(TAG, "Commande MQTT rejetée au parse (code=" +
                      String((int)res) + ")");
        return;
    }

    DataLogger::traceCommand(cmd);

    if (!CommandRouter::route(cmd.cmdId, cmd.durationMs)) {
        Console::warn(TAG, "Commande MQTT non routée : cmdId=" +
                      String((uint8_t)cmd.cmdId) +
                      " (absente de RELAYS[] ou manager non prêt)");
        return;
    }

    Console::info(TAG, "Commande MQTT acceptée : cmdId=" +
                  String((uint8_t)cmd.cmdId) +
                  " durée=" + String(cmd.durationMs) + "ms");
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

    p += "  \"csvColumns\": [\"timestamp\", \"VClock_available\", \"VClock_reliable\", "
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
// Drain 1 entrée/s de DataLogger::logBufferOut vers esp-mqtt + watchdog zombie.
// Tâche TaskManager période 1 s. Non-bloquant (latence max 2 s via
// network_timeout_ms). En cas d'échec enqueue, le payload reste dans le slot
// in-flight et sera retenté au prochain tour — aucun record perdu sur
// erreur transitoire. La backpressure globale (bursts + coupures WiFi) est
// absorbée par DataLogger::logBufferOut en amont (capacité 20, éviction FIFO).
// =============================================================================
void MqttManager::handle()
{
    if (!mqttClient) return;
    ensureMqttStarted();

    // ─── Signal MqttKo → LilyGo (evaluation quel que soit l'etat connecte) ─
    // Premier envoi apres MQTT_KO_FIRST_DELAY_MS de deconnexion continue,
    // puis repetition toutes les MQTT_KO_REPEAT_DELAY_MS tant que MQTT reste KO.
    // Les temporisations sont desarmees des qu'un MQTT_EVENT_CONNECTED survient.
    if (mqttKoDownSinceMs != 0) {
        uint32_t now      = millis();
        uint32_t downFor  = now - mqttKoDownSinceMs;
        bool     shouldSend;
        if (mqttKoLastSentMs == 0) {
            shouldSend = (downFor >= MQTT_KO_FIRST_DELAY_MS);
        } else {
            shouldSend = ((now - mqttKoLastSentMs) >= MQTT_KO_REPEAT_DELAY_MS);
        }
        if (shouldSend) {
            mqttKoSentCount++;
            mqttKoLastSentMs = now;
            Console::warn(TAG,
                "MQTT KO depuis " + String(downFor / 1000) + "s — envoi MqttKo #"
                + String(mqttKoSentCount) + " a LilyGo");
            BridgeManager::sendMqttKo();
        }
    }

    if (!mqttConnected) return;

    // ─── Slot in-flight : recharge si libre ──────────────────────────────
    // Pop un record de DataLogger::logBufferOut, format et stocke dans le slot
    // in-flight. Si le LogBufferOut est vide, rien à faire. Si le payload formaté
    // dépasse la taille du slot, warning et skip (record perdu — cas
    // pathologique uniquement si META ajoute un texte > 199 caractères).
    if (!inFlightBusy) {
        DataRecord rec;
        if (DataLogger::tryPopForPublish(rec)) {
            String csv = formatCsvPayload(rec);
            if (csv.length() >= sizeof(inFlightPayload)) {
                Console::warn(TAG, "Payload trop longue (" + String(csv.length())
                                  + " octets) pour id=" + String((uint8_t)rec.id)
                                  + " — message non publié sur MQTT");
            } else {
                strncpy(inFlightPayload, csv.c_str(), sizeof(inFlightPayload) - 1);
                inFlightPayload[sizeof(inFlightPayload) - 1] = '\0';
                inFlightId   = (uint8_t)rec.id;
                inFlightBusy = true;
            }
        }
    }

    // ─── Enqueue esp_mqtt du slot in-flight ──────────────────────────────
    if (inFlightBusy) {
        String topic = "serre/data/" + String(inFlightId);

        int msgId = esp_mqtt_client_enqueue(
            (esp_mqtt_client_handle_t)mqttClient,
            topic.c_str(),
            inFlightPayload, strlen(inFlightPayload),
            1,           // qos=1 (requis pour MQTT_EVENT_PUBLISHED)
            false,       // retain=false
            true         // store=true (requis pour que enqueue accepte)
        );

        if (msgId >= 0) {
            messagesEnqueued++;
            inFlightBusy = false;
        } else {
            Console::warn(TAG, "Enqueue échoué id=" + String(inFlightId)
                              + " — réessai au prochain tour");
        }
    }

    // Cast int32_t : tolère une race PUBACK vs enqueue sans fausse alerte.
    // Watchdog millis-based : la fenêtre de 65 min est évaluée par comparaison
    // de millis() avec l'horodatage du dernier PUBACK (watchdogSeconds).
    // Indépendant de la période d'appel de handle().
    int32_t gap = (int32_t)(messagesEnqueued - messagesPublished);
    if (gap >= (int32_t)WATCHDOG_GAP_THRESHOLD &&
        (millis() - watchdogSeconds) >= (WATCHDOG_SECONDS * 1000UL)) {
        forcedDisconnectCount++;
        Console::warn(TAG,
            "Zombie MQTT détecté (gap=" + String(gap) +
            ", " + String(WATCHDOG_SECONDS / 60) + " min sans PUBACK) — "
            "disconnect forcé (cumul depuis boot : " +
            String(forcedDisconnectCount) + ")");

        esp_mqtt_client_disconnect((esp_mqtt_client_handle_t)mqttClient);

        messagesEnqueued  = 0;
        messagesPublished = 0;
        watchdogSeconds   = millis();   // horodatage du reset post-disconnect
    }
}

// =============================================================================
// Formatage CSV 7 champs :
// timestamp,VClock_available,VClock_reliable,type,id,valueType,value
// =============================================================================
String MqttManager::formatCsvPayload(const DataRecord& record)
{
    String csv;
    csv.reserve(80);

    csv += String(record.timestamp);
    csv += ',';
    csv += String((int)record.VClock_available);
    csv += ',';
    csv += String((int)record.VClock_reliable);
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