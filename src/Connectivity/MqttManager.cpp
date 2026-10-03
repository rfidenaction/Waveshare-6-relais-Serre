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
#include "Core/DataBus.h"
#include "Config/MetaDataModel.h"
#include "Config/Neo.h"
#include "Gardener/GardenerManager.h"
#include "Gardener/ConditionalWatering.h"
#include "Sensors/OnDemandMeasure.h"
#include "Storage/HistoryQuery.h"
#include "Utils/Console.h"

#include "mqtt_client.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <variant>
#include <stdlib.h>
#include <string.h>

static const char* TAG = "MQTT";

// ─── Variables statiques ─────────────────────────────────────────────────
void*         MqttManager::mqttClient      = nullptr;
volatile bool MqttManager::mqttConnected   = false;
bool          MqttManager::mqttStarted     = false;
bool          MqttManager::schemaPublished = false;
uint32_t      MqttManager::lastSchemaPublishMs = 0;

uint8_t       MqttManager::retainedRefreshStep   = 0;
uint32_t      MqttManager::retainedRefreshStepMs = 0;

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

char   MqttManager::uiPrefs[MqttManager::UIPREFS_MAX_LEN + 1] = {};
size_t MqttManager::uiPrefsLen = 0;

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

    cfg.buffer_size = 1024;

    // Borne toutes les attentes réseau de la tâche esp_mqtt : poignée de main
    // TLS, attente du CONNACK, lectures et écritures en session. La tâche garde
    // son verrou d'API pendant ces attentes, et esp_mqtt_client_enqueue appelée
    // depuis le thread TaskManager réclame ce même verrou. Cette valeur plafonne
    // donc l'attente qu'un tour de boucle peut subir, et la boucle pilote
    // l'arrosage : jamais plus de 2 s.
    //
    // Une poignée de main ou un CONNACK plus lents font échouer la tentative de
    // connexion ; esp-mqtt en relance une une dizaine de secondes plus tard.
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

    // LittleFS est monté dans setup(), bien avant loopInit() : le fichier est
    // lisible ici. loadFamilyNames() reste appelée depuis main.cpp, où elle
    // était déjà.
    loadUiPrefs();

    Console::info(TAG, "Client MQTT configuré (en attente WiFi STA)");
    Console::info(TAG, "Broker: " + String(MQTT_BROKER_URI));
    Console::info(TAG, "Client ID: " + String(MQTT_CLIENT_ID));
    Console::info(TAG, "Slot in-flight : 1 item (backpressure via DataBus::mqttQueue)");
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

        if (!schemaPublished) {
            publishSchema();
            publishNeo();
            schemaPublished = true;
        }

        esp_mqtt_client_subscribe(
            (esp_mqtt_client_handle_t)mqttClient,
            "serre/cmd", 1
        );
        Console::info(TAG, "Abonné à serre/cmd");

        esp_mqtt_client_subscribe(
            (esp_mqtt_client_handle_t)mqttClient,
            GardenerManager::GARDENER_TOPIC_FROM_USER, 1
        );
        Console::info(TAG, "Abonné à " + String(GardenerManager::GARDENER_TOPIC_FROM_USER));

        GardenerManager::requestStatePublish();

        esp_mqtt_client_subscribe(
            (esp_mqtt_client_handle_t)mqttClient,
            ConditionalWatering::CONDITIONAL_TOPIC_FROM_USER, 1
        );
        Console::info(TAG, "Abonné à "
                     + String(ConditionalWatering::CONDITIONAL_TOPIC_FROM_USER));

        ConditionalWatering::requestStatePublish();

        esp_mqtt_client_subscribe(
            (esp_mqtt_client_handle_t)mqttClient,
            "serre/families/rename", 1
        );
        Console::info(TAG, "Abonné à serre/families/rename");

        esp_mqtt_client_subscribe(
            (esp_mqtt_client_handle_t)mqttClient,
            OnDemandMeasure::ONDEMAND_TOPIC_FROM_USER, 1
        );
        Console::info(TAG, "Abonné à "
                     + String(OnDemandMeasure::ONDEMAND_TOPIC_FROM_USER));

        esp_mqtt_client_subscribe(
            (esp_mqtt_client_handle_t)mqttClient,
            HistoryQuery::HISTORY_TOPIC_FROM_USER, 1
        );
        Console::info(TAG, "Abonné à "
                     + String(HistoryQuery::HISTORY_TOPIC_FROM_USER));

        esp_mqtt_client_subscribe(
            (esp_mqtt_client_handle_t)mqttClient,
            MQTT_PING_TOPIC_FROM_USER, 1
        );
        Console::info(TAG, "Abonné à " + String(MQTT_PING_TOPIC_FROM_USER));

        esp_mqtt_client_subscribe(
            (esp_mqtt_client_handle_t)mqttClient,
            MQTT_UIPREFS_TOPIC_FROM_USER, 1
        );
        Console::info(TAG, "Abonné à " + String(MQTT_UIPREFS_TOPIC_FROM_USER));

        publishUiPrefs();

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
        // Routage par topic : Gardener FromUser, Conditional FromUser,
        // Family rename, ou commande serre/cmd
        if (event->topic && event->topic_len > 0) {
            static const char GD_TOPIC[] = "serre/gardener/FromUser";
            static const int  GD_LEN     = sizeof(GD_TOPIC) - 1;
            if (event->topic_len == GD_LEN &&
                memcmp(event->topic, GD_TOPIC, GD_LEN) == 0) {
                GardenerManager::onGardenerMessage(event->data, event->data_len);
                break;
            }

            static const char CW_TOPIC[] = "serre/conditional/FromUser";
            static const int  CW_LEN     = sizeof(CW_TOPIC) - 1;
            if (event->topic_len == CW_LEN &&
                memcmp(event->topic, CW_TOPIC, CW_LEN) == 0) {
                ConditionalWatering::onConditionalMessage(event->data, event->data_len);
                break;
            }

            static const char FAM_TOPIC[] = "serre/families/rename";
            static const int  FAM_LEN     = sizeof(FAM_TOPIC) - 1;
            if (event->topic_len == FAM_LEN &&
                memcmp(event->topic, FAM_TOPIC, FAM_LEN) == 0) {
                handleFamilyRename(event->data, event->data_len);
                break;
            }

            static const char OND_TOPIC[] = "serre/ondemand/FromUser";
            static const int  OND_LEN     = sizeof(OND_TOPIC) - 1;
            if (event->topic_len == OND_LEN &&
                memcmp(event->topic, OND_TOPIC, OND_LEN) == 0) {
                // Une demande de mesure n'a de sens qu'au moment où elle est
                // émise : c'est un appui, pas un état. Un message retenu sur ce
                // topic serait redélivré par le broker à CHAQUE abonnement,
                // donc à chaque reconnexion de la carte, et déclencherait une
                // lecture que personne n'a demandée — avec tous ses effets
                // normaux, dont l'alerte secteur sur AcPower.
                //
                // L'interface publie sans retain, mais le garde est côté carte :
                // il protège quel que soit l'émetteur (client tiers, publication
                // manuelle depuis une console MQTT).
                if (event->retain) {
                    Console::warn(TAG, "Demande de mesure retenue ignorée sur "
                                       + String(OND_TOPIC));
                    break;
                }
                OnDemandMeasure::onRequest(event->data, event->data_len);
                break;
            }

            static const char HIST_TOPIC[] = "serre/history/FromUser";
            static const int  HIST_LEN     = sizeof(HIST_TOPIC) - 1;
            if (event->topic_len == HIST_LEN &&
                memcmp(event->topic, HIST_TOPIC, HIST_LEN) == 0) {
                // Même garde que pour la mesure à la demande : une demande
                // d'historique est un appui, pas un état. Un message retenu sur
                // ce topic serait redélivré à chaque abonnement, donc à chaque
                // reconnexion de la carte, et lancerait un scan de la flash que
                // personne n'a demandé.
                if (event->retain) {
                    Console::warn(TAG, "Demande d'historique retenue ignorée sur "
                                       + String(HIST_TOPIC));
                    break;
                }
                HistoryQuery::onRequest(event->data, event->data_len);
                break;
            }

            static const char PING_TOPIC[] = "serre/ping/FromUser";
            static const int  PING_LEN     = sizeof(PING_TOPIC) - 1;
            if (event->topic_len == PING_LEN &&
                memcmp(event->topic, PING_TOPIC, PING_LEN) == 0) {
                publishPong();
                break;
            }

            static const char PREFS_TOPIC[] = "serre/uiprefs/FromUser";
            static const int  PREFS_LEN     = sizeof(PREFS_TOPIC) - 1;
            if (event->topic_len == PREFS_LEN &&
                memcmp(event->topic, PREFS_TOPIC, PREFS_LEN) == 0) {
                handleUiPrefs(event->data, event->data_len);
                break;
            }
        }
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
//   1. DataBus::parseCommand       : décode et valide (fonction pure).
//   2. DataBus::publish             : horodate + distribue + route via RELAYS[].
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

    BusItem item;
    auto res = DataBus::parseCommand(
        event->data, (size_t)event->data_len, item);
    if (res != CommandParseResult::OK) {
        Console::warn(TAG, "Commande MQTT rejetée au parse (code=" +
                      String((int)res) + ")");
        return;
    }

    DataBus::publish(item);

    Console::info(TAG, "Commande MQTT acceptée : id=" +
                  String((uint8_t)item.id) +
                  " durée=" + String((uint32_t)(item.valueFloat * 1000.0f)) + "ms");
}

// =============================================================================
// Réponse au ping de l'interface. Appelée depuis l'event handler, donc dans le
// thread esp_mqtt : le pong part sur la socket avant même le retour du handler.
//
// retain=false, à la différence de tous les autres passe-plats : un pong est la
// preuve qu'une carte est vivante à l'instant où elle répond, pas un état. Le
// broker le rejouerait à chaque abonnement et l'interface afficherait la serre
// en ligne alors que la carte serait éteinte — l'exact contraire du but.
// =============================================================================
void MqttManager::publishPong()
{
    esp_mqtt_client_publish(
        (esp_mqtt_client_handle_t)mqttClient,
        MQTT_PING_TOPIC_TO_USER,
        "pong", 4,
        1, false
    );
    Console::info(TAG, "Pong publié sur " + String(MQTT_PING_TOPIC_TO_USER));
}

// =============================================================================
// Publication du schéma JSON (retain) — une seule fois au boot.
// =============================================================================
void MqttManager::publishSchema()
{
    String json = buildSchemaJson();

    int msgId = esp_mqtt_client_enqueue(
        (esp_mqtt_client_handle_t)mqttClient,
        MQTT_SCHEMA_TOPIC,
        json.c_str(), json.length(),
        1,      // qos=1
        true,   // retain=true
        true    // store=true (requis pour que enqueue accepte)
    );

    // Réarmé sur la tentative et non sur le succès : un échec ne doit pas
    // provoquer une republication à chaque tour de handle().
    lastSchemaPublishMs = millis();

    if (msgId >= 0) {
        Console::info(TAG, "Schéma mis en file pour " + String(MQTT_SCHEMA_TOPIC)
                     + " (" + String(json.length()) + " octets)");
    } else {
        Console::error(TAG, "Échec mise en file du schéma");
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
    // y compris ceux absents de META (CommandManual, CommandAuto,
    // CommandConditional). Libellés fournis par typeLabel()
    // (MetaDataModel.h, source de vérité unique).
    p += "  \"dataTypes\": [\n";
    bool firstType = true;
    for (uint8_t t = 0; t <= (uint8_t)DataType::CommandConditional; t++) {
        if (!firstType) p += ",\n";
        firstType = false;
        p += "    {\"id\": "; p += t;
        p += ", \"label\": \"";
        p += jsonEscape(typeLabel((DataType)t));
        p += "\"}";
    }
    p += "\n  ],\n";

    // Table DataId (depuis META).
    p += "  \"dataIds\": [\n";
    for (size_t i = 0; i < META_COUNT; i++) {
        const DataMeta& m = META[i];

        p += "    {\"id\": "; p += (uint8_t)m.id;
        p += ", \"label\": \""; p += jsonEscape(m.label); p += "\"";
        p += ", \"unit\": \"";  p += jsonEscape(m.unit);  p += "\"";

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
                    p += jsonEscape(m.stateLabels[s]);
                }
                p += "\"}";
            }
            p += "]";
        }

        p += "}";
        if (i < META_COUNT - 1) p += ",";
        p += "\n";
    }
    p += "  ],\n";

    p += "  \"families\": [";
    for (uint8_t i = 0; i < FAMILY_COUNT; i++) {
        if (i > 0) p += ", ";
        p += "\"";
        p += jsonEscape(familyNames[i]);
        p += "\"";
    }
    p += "]\n";

    // Plus de liste measurableIds ici : elle disait quelles données la carte
    // sait mesurer à la demande, ce que NEO dit déjà. Une donnée est
    // mesurable si son entrée NEO porte une grandeur, puisque seuls les
    // modules capteurs en déclarent une, et qu'ils fournissent du même geste
    // le pointeur de mesure. Deux messages, deux rôles, aucun recouvrement.

    p += "}";

    return p;
}

// =============================================================================
// Publication de la table NEO (retain), sur son propre topic.
// =============================================================================
void MqttManager::publishNeo()
{
    String json = buildNeoJson();

    int msgId = esp_mqtt_client_enqueue(
        (esp_mqtt_client_handle_t)mqttClient,
        MQTT_NEO_TOPIC,
        json.c_str(), json.length(),
        1,      // qos=1
        true,   // retain=true
        true    // store=true (requis pour que enqueue accepte)
    );

    if (msgId >= 0) {
        Console::info(TAG, "NEO mis en file pour " + String(MQTT_NEO_TOPIC)
                     + " (" + String(json.length()) + " octets, "
                     + String(Neo::count()) + " entrées)");
    } else {
        Console::error(TAG, "Échec mise en file de NEO");
    }
}

// =============================================================================
// Génération de la table NEO en JSON.
//
// Deux tables, deux messages : celui-ci ne reprend AUCUN champ de META. Il ne
// porte que des DataId, qui sont les clés de jointure, et les caractéristiques
// matérielles que META ne peut pas décrire. L'interface joint les deux par id.
//
// Les champs matériels sont omis quand ils ne s'appliquent pas : pas d'adresse
// pour une entité système, pas de canal ni d'id lié pour un capteur.
// =============================================================================
String MqttManager::buildNeoJson()
{
    String p;
    p.reserve(1536);

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

    // Légendes des deux énumérations — l'interface n'a aucune valeur à coder
    // en dur, exactement comme pour dataTypes dans le schéma.
    p += "  \"grandeurs\": [\n";
    for (uint8_t g = 0; g <= (uint8_t)Grandeur::Debit; g++) {
        if (g > 0) p += ",\n";
        p += "    {\"id\": "; p += g;
        p += ", \"label\": \"";
        p += jsonEscape(grandeurLabel((Grandeur)g));
        p += "\"}";
    }
    p += "\n  ],\n";

    p += "  \"concernes\": [\n";
    for (uint8_t c = 0; c <= (uint8_t)Concerne::Secteur; c++) {
        if (c > 0) p += ",\n";
        p += "    {\"id\": "; p += c;
        p += ", \"label\": \"";
        p += jsonEscape(concerneLabel((Concerne)c));
        p += "\"}";
    }
    p += "\n  ],\n";

    p += "  \"entries\": [\n";
    for (uint8_t i = 0; i < Neo::count(); i++) {
        const NeoEntry& e = Neo::at(i);

        p += "    {\"id\": ";        p += (uint8_t)e.id;
        p += ", \"grandeur\": ";     p += (uint8_t)e.grandeur;
        p += ", \"concerne\": ";     p += (uint8_t)e.concerne;

        if (e.rs485Address != 0) {
            p += ", \"addr\": ";     p += e.rs485Address;
        }

        if (e.relayCh != 0) {
            p += ", \"ch\": ";       p += e.relayCh;
            p += ", \"lie\": ";      p += (uint8_t)e.idLie;
        }

        p += "}";
        if (i < Neo::count() - 1) p += ",";
        p += "\n";
    }
    p += "  ]\n";

    p += "}";

    return p;
}

// =============================================================================
// Drain de DataBus::mqttQueue vers esp-mqtt + watchdog zombie.
// Tâche TaskManager période 200 ms. Aucune écriture sur la socket : tout sort
// par l'outbox esp_mqtt. Un tour peut néanmoins attendre le verrou d'API que la
// tâche esp_mqtt garde pendant ses propres attentes réseau ; cette attente est
// bornée par cfg.network_timeout_ms (2 s, voir init()) et chaque enqueue du tour
// la risque une fois. C'est la seule attente réseau que subit la boucle. En cas d'échec
// enqueue, le payload reste dans le slot in-flight et sera retenté au prochain
// tour — aucun item perdu sur erreur transitoire. La backpressure globale
// (bursts + coupures WiFi) est absorbée par DataBus::mqttQueue en amont
// (capacité 30, éviction FIFO).
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

    // ─── Rafraîchissement périodique des messages retenus ────────────────
    // Évalué uniquement une fois connecté : une coupure ne consomme donc pas
    // l'échéance, la republication a lieu dès le retour du lien. Soustraction
    // non signée, insensible au débordement de millis(). publishSchema()
    // réarme l'échéance, un renommage de famille ou une reconnexion la
    // repoussent donc d'autant.
    //
    // L'échéance n'émet que le schéma : NEO et les deux programmations suivent,
    // une par RETAINED_REFRESH_SPACING_MS. Le second bloc ne peut pas se
    // déclencher dans la même passe que le premier, qui vient de poser
    // retainedRefreshStepMs à l'instant courant. Une coupure du lien gèle la
    // séquence sur le return ci-dessus et elle reprend au retour du lien.
    if ((millis() - lastSchemaPublishMs) >= RETAINED_REFRESH_MS) {
        Console::info(TAG, "Rafraîchissement des messages retenus — schéma, puis NEO et programmations espacés");
        publishSchema();
        retainedRefreshStep   = 1;
        retainedRefreshStepMs = millis();
    }

    if (retainedRefreshStep != 0 &&
        (millis() - retainedRefreshStepMs) >= RETAINED_REFRESH_SPACING_MS) {
        switch (retainedRefreshStep) {
            case 1: publishNeo();                               break;
            case 2: GardenerManager::requestStatePublish();      break;
            case 3: ConditionalWatering::requestStatePublish();  break;
        }
        retainedRefreshStepMs = millis();
        retainedRefreshStep   = (retainedRefreshStep < 3) ? retainedRefreshStep + 1 : 0;
    }

    // ─── Slot in-flight : recharge si libre ──────────────────────────────
    // Pop un BusItem de DataBus::mqttQueue, formate en CSV et stocke dans le
    // slot in-flight. Si la queue est vide, rien à faire. Si le payload formaté
    // dépasse la taille du slot, warning et skip (item perdu — cas
    // pathologique uniquement si META ajoute un texte > 199 caractères).
    if (!inFlightBusy) {
        BusItem item;
        if (DataBus::tryPopMqtt(item)) {
            String csv = formatCsvPayload(item);
            if (csv.length() >= sizeof(inFlightPayload)) {
                Console::warn(TAG, "Payload trop longue (" + String(csv.length())
                                  + " octets) pour id=" + String((uint8_t)item.id)
                                  + " — message non publié sur MQTT");
            } else {
                strncpy(inFlightPayload, csv.c_str(), sizeof(inFlightPayload) - 1);
                inFlightPayload[sizeof(inFlightPayload) - 1] = '\0';
                inFlightId   = (uint8_t)item.id;
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
            true,        // retain=true
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
String MqttManager::formatCsvPayload(const BusItem& item)
{
    String csv;
    csv.reserve(80);

    csv += String(item.timestamp);
    csv += ',';
    csv += String((int)item.VClock_available);
    csv += ',';
    csv += String((int)item.VClock_reliable);
    csv += ',';
    csv += String((uint8_t)item.type);
    csv += ',';
    csv += String((uint8_t)item.id);

    if (item.valueKind == 0) {
        csv += ",0,";
        csv += String(item.valueFloat, 3);
    } else {
        csv += ",1,";
        csv += escapeCSV(String(item.valueText));
    }

    return csv;
}

// =============================================================================
// Publication de l'état Gardener (retain sur serre/gardener/ToUser).
// Passe-plat : reçoit le JSON prêt de GardenerManager.
// =============================================================================
void MqttManager::publishGardenerWateringState(const char* payload, size_t len)
{
    if (!mqttClient || !mqttConnected) return;

    int msgId = esp_mqtt_client_enqueue(
        (esp_mqtt_client_handle_t)mqttClient,
        GardenerManager::GARDENER_TOPIC_TO_USER,
        payload, len,
        1,      // qos=1
        true,   // retain=true
        true    // store=true (requis pour que enqueue accepte)
    );

    if (msgId >= 0) {
        Console::info(TAG, "Gardener state mis en file pour "
                     + String(GardenerManager::GARDENER_TOPIC_TO_USER)
                     + " (" + String(len) + " octets)");
    } else {
        Console::error(TAG, "Échec mise en file du Gardener state");
    }
}

// =============================================================================
// Publication de l'état de l'arrosage conditionnel
// (retain sur serre/conditional/ToUser).
// Passe-plat : reçoit le JSON prêt de ConditionalWatering.
// =============================================================================
void MqttManager::publishConditionalState(const char* payload, size_t len)
{
    if (!mqttClient || !mqttConnected) return;

    int msgId = esp_mqtt_client_enqueue(
        (esp_mqtt_client_handle_t)mqttClient,
        ConditionalWatering::CONDITIONAL_TOPIC_TO_USER,
        payload, len,
        1,      // qos=1
        true,   // retain=true
        true    // store=true (requis pour que enqueue accepte)
    );

    if (msgId >= 0) {
        Console::info(TAG, "Conditional state mis en file pour "
                     + String(ConditionalWatering::CONDITIONAL_TOPIC_TO_USER)
                     + " (" + String(len) + " octets)");
    } else {
        Console::error(TAG, "Échec mise en file du Conditional state");
    }
}

// =============================================================================
// Publication d'une réponse d'historique (serre/history/ToUser).
// Passe-plat : reçoit le JSON prêt de HistoryQuery.
//
// retain=false : voir MqttManager.h. Une réponse périmée redélivrée à chaque
// reconnexion afficherait un graphique faux.
// =============================================================================
void MqttManager::publishHistory(const char* payload, size_t len)
{
    if (!mqttClient || !mqttConnected) return;

    int msgId = esp_mqtt_client_enqueue(
        (esp_mqtt_client_handle_t)mqttClient,
        HistoryQuery::HISTORY_TOPIC_TO_USER,
        payload, len,
        1,      // qos=1
        false,  // retain=false
        true    // store=true (requis pour que enqueue accepte)
    );

    if (msgId >= 0) {
        Console::info(TAG, "Historique mis en file pour "
                     + String(HistoryQuery::HISTORY_TOPIC_TO_USER)
                     + " (" + String(len) + " octets)");
    } else {
        Console::error(TAG, "Échec mise en file de l'historique");
    }
}

// =============================================================================
bool MqttManager::isMqttConnected()
{
    return mqttConnected;
}

// =============================================================================
// Familles — noms utilisateur pour les 6 vannes/capteurs
// =============================================================================

char MqttManager::familyNames[FAMILY_COUNT][FAMILY_NAME_MAX + 1] = {};

void MqttManager::loadFamilyNames()
{
    for (uint8_t i = 0; i < FAMILY_COUNT; i++) {
        strncpy(familyNames[i], "Famille", FAMILY_NAME_MAX);
        familyNames[i][FAMILY_NAME_MAX] = '\0';
    }

    File f = LittleFS.open("/families.json", "r");
    if (!f) {
        Console::info(TAG, "Fichier /families.json absent — noms par défaut");
        return;
    }

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Console::warn(TAG, "families.json malformé : " + String(err.c_str()));
        return;
    }

    JsonArray arr = doc.as<JsonArray>();
    for (uint8_t i = 0; i < FAMILY_COUNT && i < arr.size(); i++) {
        const char* name = arr[i] | "Famille";
        strncpy(familyNames[i], name, FAMILY_NAME_MAX);
        familyNames[i][FAMILY_NAME_MAX] = '\0';
    }

    Console::info(TAG, "Familles chargées depuis /families.json");
}

bool MqttManager::saveFamilyNames()
{
    File f = LittleFS.open("/families.tmp", "w");
    if (!f) {
        Console::error(TAG, "Échec ouverture /families.tmp");
        return false;
    }

    f.print("[");
    for (uint8_t i = 0; i < FAMILY_COUNT; i++) {
        if (i > 0) f.print(",");
        f.print("\"");
        f.print(familyNames[i]);
        f.print("\"");
    }
    f.print("]");
    f.close();

    if (!LittleFS.rename("/families.tmp", "/families.json")) {
        Console::error(TAG, "Échec rename /families.tmp → /families.json");
        return false;
    }

    return true;
}

void MqttManager::handleFamilyRename(const char* data, int len)
{
    if (len <= 0 || len > 128) return;

    char buf[129];
    memcpy(buf, data, len);
    buf[len] = '\0';

    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, buf);
    if (err) {
        Console::warn(TAG, "Family rename JSON malformé : " + String(err.c_str()));
        return;
    }

    int id = doc["id"] | 0;
    const char* name = doc["name"] | "";

    if (id < 1 || id > FAMILY_COUNT) {
        Console::warn(TAG, "Family rename — id hors bornes : " + String(id));
        return;
    }

    strncpy(familyNames[id - 1], name, FAMILY_NAME_MAX);
    familyNames[id - 1][FAMILY_NAME_MAX] = '\0';

    if (saveFamilyNames()) {
        Console::info(TAG, "Famille " + String(id) + " renommée : " + String(name));
    }

    schemaPublished = false;
    publishSchema();
    schemaPublished = true;
}

// =============================================================================
// Préférences d'affichage de l'interface — voir MqttManager.h
//
// Mémoire commune aux téléphones, opaque pour la carte. Traitées depuis le
// thread esp_mqtt, comme le renommage de famille juste au-dessus : il s'agit
// d'écrire un petit fichier sur commande de l'utilisateur, pas de parcourir
// une structure qui pilote des vannes.
//
// Délibérément hors du schéma, à la différence des noms de familles :
// schemaSignature() compare tout le schéma sauf "generated", donc y loger ces
// préférences ferait reconstruire les trois chapitres de l'interface à chaque
// renommage de boîtier, refermant tous les dépliants ouverts.
// =============================================================================

void MqttManager::loadUiPrefs()
{
    uiPrefs[0] = '\0';
    uiPrefsLen = 0;

    File f = LittleFS.open("/uiprefs.json", "r");
    if (!f) {
        Console::info(TAG, "Fichier /uiprefs.json absent — préférences d'affichage par défaut");
        return;
    }

    size_t size = f.size();
    if (size == 0 || size > UIPREFS_MAX_LEN) {
        Console::warn(TAG, "/uiprefs.json de taille invalide (" + String(size)
                          + " octets) — ignoré");
        f.close();
        return;
    }

    uiPrefsLen = f.readBytes(uiPrefs, size);
    uiPrefs[uiPrefsLen] = '\0';
    f.close();

    Console::info(TAG, "Préférences d'affichage chargées ("
                      + String(uiPrefsLen) + " octets)");
}

bool MqttManager::saveUiPrefs()
{
    File f = LittleFS.open("/uiprefs.tmp", "w");
    if (!f) {
        Console::error(TAG, "Échec ouverture /uiprefs.tmp en écriture");
        return false;
    }

    size_t written = f.write((const uint8_t*)uiPrefs, uiPrefsLen);
    f.close();

    if (written != uiPrefsLen) {
        Console::error(TAG, "Écriture partielle /uiprefs.tmp (" + String(written)
                          + "/" + String(uiPrefsLen) + ")");
        return false;
    }

    if (!LittleFS.rename("/uiprefs.tmp", "/uiprefs.json")) {
        Console::error(TAG, "Échec rename /uiprefs.tmp → /uiprefs.json");
        return false;
    }

    return true;
}

// Le message reçu remplace intégralement l'état mémorisé : l'interface publie
// l'objet complet à chaque modification, il n'y a pas d'ordre unitaire à
// fusionner.
void MqttManager::handleUiPrefs(const char* data, int len)
{
    if (len <= 0 || (size_t)len > UIPREFS_MAX_LEN) {
        Console::warn(TAG, "Préférences d'affichage rejetées — taille "
                          + String(len) + " octets");
        return;
    }

    // Contrôle de forme, pas de sens : on vérifie seulement que c'est du JSON.
    // Sans ce garde-fou, un émetteur tiers pourrait figer en flash un contenu
    // que l'interface n'arriverait plus jamais à relire.
    {
        DynamicJsonDocument probe(UIPREFS_MAX_LEN + 512);
        if (deserializeJson(probe, data, (size_t)len) != DeserializationError::Ok) {
            Console::warn(TAG, "Préférences d'affichage rejetées — JSON malformé");
            return;
        }
    }

    memcpy(uiPrefs, data, len);
    uiPrefs[len] = '\0';
    uiPrefsLen   = (size_t)len;

    if (saveUiPrefs()) {
        Console::info(TAG, "Préférences d'affichage enregistrées ("
                          + String(uiPrefsLen) + " octets)");
    }

    publishUiPrefs();
}

// Retain : c'est un état, et il doit parvenir à un téléphone qui se connecte
// longtemps après la dernière modification.
void MqttManager::publishUiPrefs()
{
    if (!mqttClient || !mqttConnected) return;

    // Rien en flash : on publie quand même un objet vide, pour que l'interface
    // sache qu'elle a reçu la réponse de la carte et applique ses défauts,
    // plutôt que d'attendre indéfiniment un message qui ne viendrait pas.
    const char* payload = (uiPrefsLen > 0) ? uiPrefs   : "{}";
    size_t      len     = (uiPrefsLen > 0) ? uiPrefsLen : 2;

    int msgId = esp_mqtt_client_publish(
        (esp_mqtt_client_handle_t)mqttClient,
        MQTT_UIPREFS_TOPIC_TO_USER,
        payload, len,
        1, true
    );

    if (msgId >= 0) {
        Console::info(TAG, "Préférences d'affichage publiées sur "
                     + String(MQTT_UIPREFS_TOPIC_TO_USER)
                     + " (" + String(len) + " octets)");
    } else {
        Console::error(TAG, "Échec publication des préférences d'affichage");
    }
}