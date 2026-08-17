// src/Connectivity/MqttManager.h
#pragma once

#include <Arduino.h>
#include "Config/MetaDataModel.h"

struct BusItem;  // forward declaration (défini dans Core/DataBus.h)

// Client MQTT non-bloquant (esp_mqtt natif ESP-IDF 4.4.4).
// esp-mqtt gère sa propre tâche FreeRTOS ; MqttManager se limite à un slot
// « in-flight » (pour retry sur échec d'enqueue) et un watchdog zombie pour
// la robustesse face aux coupures et aux brokers muets (pas de PUBACK).
//
// La backpressure amont est portée par DataBus::mqttQueue (queue FreeRTOS,
// capacité 30, éviction FIFO) : c'est elle qui absorbe les bursts métier
// et les coupures WiFi.
//
// Intégration :
//  - init() appelé dans loopInit() après WiFiManager
//  - handle() en tâche TaskManager période 200 ms
//  - handle() pop 1 item de DataBus::mqttQueue, format CSV + enqueue esp_mqtt
//  - setOnPublishSuccess(cb) : callback externe sur PUBACK (BridgeManager)

class MqttManager {
public:
    // ─── Paramètres ajustables (rythme métier) ───────────────────────────
    //
    // Mécanisme global
    // ────────────────
    // - handle() pop 1 item de DataBus::mqttQueue dès que mqttConnected
    //   == true, le formate en CSV et l'enqueue dans esp_mqtt. Chaque
    //   enqueue incrémente messagesEnqueued.
    // - Si l'enqueue échoue (esp_mqtt saturée / déconnectée), le record
    //   formaté reste dans inFlightPayload et sera réémis au tour suivant
    //   (pas de perte sur erreur transitoire).
    // - À chaque PUBACK reçu, les compteurs ET le décompte watchdogSeconds
    //   sont remis à zéro → le broker est prouvé réactif.
    // - Si le broker devient muet (aucun PUBACK), messagesEnqueued s'accumule
    //   pendant que watchdogSeconds décompte. Quand les DEUX conditions
    //   "gap ≥ seuil" et "watchdog expiré" sont réunies simultanément, on
    //   force un disconnect ; esp-mqtt reconnecte seule ~10 s plus tard.
    //
    // Rôle de chaque paramètre
    // ────────────────────────
    // WATCHDOG_GAP_THRESHOLD (enqueues sans PUBACK) : seuil à partir duquel
    //   le broker est suspecté muet. Plus bas = alerte plus sensible, sans
    //   conséquence tant que le watchdog n'expire pas. Règle : proche de la
    //   taille d'un burst typique (détection en 1 ou 2 bursts silencieux
    //   consécutifs selon la taille réelle des bursts).
    //
    // WATCHDOG_SECONDS (secondes) : fenêtre de patience après que le seuil
    //   de gap a été franchi, avant de forcer un disconnect. Si un seul
    //   PUBACK revient pendant cette fenêtre, tout est reset et aucune
    //   action n'est prise. Règle d'or : LÉGÈREMENT SUPÉRIEUR à l'intervalle
    //   de bursts (ex. 65 min pour un cycle ~60 min). Le burst suivant sert
    //   ainsi de « sonde naturelle » : s'il produit un PUBACK, le broker
    //   est prouvé sain et on évite un disconnect inutile. Ne décompte QUE
    //   quand mqttConnected == true (une coupure WiFi ne consomme pas la
    //   fenêtre).
    //
    static constexpr uint32_t WATCHDOG_GAP_THRESHOLD = 7;      // enqueues sans PUBACK
    static constexpr uint32_t WATCHDOG_SECONDS       = 3900;   // 65 min

    // RETAINED_REFRESH_MS : période de republication des trois éléments à
    //   durée de vie longue (schéma, programmation horaire, règles
    //   conditionnelles). Le plan HiveMQ Cloud Serverless expire les messages
    //   retenus au bout de 3 jours ; sur une installation stable, où plus rien
    //   n'est publié spontanément, un téléphone neuf ne trouverait donc plus
    //   ni schéma ni programmations. Valeur très en deçà des 72 h afin de
    //   tolérer deux échéances manquées consécutives.
    static constexpr uint32_t RETAINED_REFRESH_MS = 24UL * 3600UL * 1000UL;  // 24 h

    static void init();
    static void ensureMqttStarted();
    static bool isMqttConnected();
    static void handle();
    static void setOnPublishSuccess(void (*callback)());

    // Publication de l'état Gardener (retain sur serre/gardener/ToUser).
    // Passe-plat : reçoit le JSON prêt de GardenerManager.
    static void publishGardenerWateringState(const char* payload, size_t len);

    // Publication de l'état de l'arrosage conditionnel
    // (retain sur serre/conditional/ToUser).
    // Passe-plat : reçoit le JSON prêt de ConditionalWatering.
    static void publishConditionalState(const char* payload, size_t len);

    // Publication d'une réponse d'historique (serre/history/ToUser).
    // Passe-plat : reçoit le JSON prêt de HistoryQuery.
    //
    // SANS retain, à la différence des trois autres passe-plats : une réponse
    // d'historique est la réponse à une question posée à un instant donné, pas
    // un état du système. Retenue, elle serait redélivrée périmée à chaque
    // reconnexion d'un téléphone et afficherait un graphique obsolète.
    //
    // Emise par enqueue et non par publish : appelée depuis le thread
    // TaskManager, qui pilote les vannes, elle ne doit pas attendre la socket.
    static void publishHistory(const char* payload, size_t len);

    // ─── Familles (noms utilisateur pour les 6 vannes/capteurs) ──────────
    static constexpr uint8_t FAMILY_COUNT    = 6;
    static constexpr uint8_t FAMILY_NAME_MAX = 24;

    // Charge /families.json au boot. Si absent, initialise "Famille".
    static void loadFamilyNames();

private:
    static void* mqttClient;
    static volatile bool mqttConnected;
    static bool mqttStarted;
    static bool schemaPublished;

    // Horodatage millis() de la dernière publication du schéma, réarmé par
    // publishSchema() quelle que soit son origine (connexion, renommage de
    // famille, échéance périodique). Sert d'échéance à RETAINED_REFRESH_MS.
    static uint32_t lastSchemaPublishMs;

    static void mqttEventHandler(void* handlerArgs, const char* base, int32_t eventId, void* eventData);
    // serre/cmd (CSV 7 champs) : DataBus::parseCommand → DataBus::publish
    static void dispatchCommand(void* eventData);
    static void publishOnline();
    static void publishSchema();
    static String buildSchemaJson();
    static String formatCsvPayload(const BusItem& item);

    static void (*_onPublishSuccess)();

    // Slot « in-flight » : un BusItem déjà formaté en CSV, en attente de
    // succès d'enqueue esp_mqtt. Si l'enqueue courant échoue, on retente
    // le même payload au tour suivant — aucun item n'est perdu sur
    // erreur transitoire. La backpressure globale (bursts, coupures WiFi)
    // est portée par DataBus::mqttQueue en amont.
    static char    inFlightPayload[200];
    static uint8_t inFlightId;
    static bool    inFlightBusy;

    // Watchdog zombie MQTT.
    // Tout PUBACK remet les trois compteurs à zéro. handle() décrémente
    // watchdogSeconds tant que mqttConnected. Si gap >= seuil ET
    // watchdogSeconds == 0 → esp_mqtt_client_disconnect().
    // volatile : cross-thread TaskManager vs esp-mqtt. uint32_t atomique ESP32.
    static volatile uint32_t messagesEnqueued;
    static volatile uint32_t messagesPublished;
    static volatile uint32_t watchdogSeconds;
    static uint32_t forcedDisconnectCount;  // diag cumul depuis boot

    // ─── Signal MqttKo (Waveshare → LilyGo via BridgeManager) ────────────
    // mqttKoDownSinceMs : horodatage millis() du debut de la deconnexion
    //   courante. Arme (≠ 0) au premier MQTT_EVENT_DISCONNECTED qui suit
    //   une periode connectee, desarme (= 0) a chaque MQTT_EVENT_CONNECTED.
    // mqttKoLastSentMs : horodatage millis() du dernier MqttKo envoye
    //   dans l'episode de deconnexion courant. Reset a chaque reconnexion.
    // Ces deux champs ne sont ecrits QUE depuis handle() / event handler
    // (thread esp_mqtt) ; lus uniquement dans handle() pour la decision
    // d'envoi. Pas de cross-thread sensible (ecritures aux transitions
    // connected/disconnected, lecture periodique 1 Hz).
    static uint32_t mqttKoDownSinceMs;
    static uint32_t mqttKoLastSentMs;
    static uint32_t mqttKoSentCount;    // diag cumul depuis boot

    // ─── Familles ────────────────────────────────────────────────────────
    static char familyNames[FAMILY_COUNT][FAMILY_NAME_MAX + 1];
    static bool saveFamilyNames();
    static void handleFamilyRename(const char* data, int len);
};