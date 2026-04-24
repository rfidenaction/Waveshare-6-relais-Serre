// src/Connectivity/MqttManager.h
#pragma once

#include <Arduino.h>
#include "Storage/DataLogger.h"

// Client MQTT non-bloquant (esp_mqtt natif ESP-IDF 4.4.4).
// esp-mqtt gère sa propre tâche FreeRTOS ; MqttManager se limite à un slot
// « in-flight » (pour retry sur échec d'enqueue) et un watchdog zombie pour
// la robustesse face aux coupures et aux brokers muets (pas de PUBACK).
//
// Le tampon FIFO amont historique a migré dans DataLogger::logBufferOut (queue
// FreeRTOS) : c'est lui qui absorbe les bursts métier et les coupures WiFi.
//
// Intégration :
//  - init() appelé dans loopInit() après WiFiManager
//  - handle() en tâche TaskManager période 1 s
//  - handle() pop 1 record/s de DataLogger::logBufferOut, format + enqueue esp_mqtt
//  - setOnPublishSuccess(cb) : callback externe sur PUBACK (BridgeManager)

class MqttManager {
public:
    // ─── Paramètres ajustables (rythme métier) ───────────────────────────
    //
    // Mécanisme global
    // ────────────────
    // - handle() pop 1 record/s de DataLogger::logBufferOut dès que mqttConnected
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

    static void init();
    static void ensureMqttStarted();
    static bool isMqttConnected();
    static void handle();
    static void setOnPublishSuccess(void (*callback)());

private:
    static void* mqttClient;
    static volatile bool mqttConnected;
    static bool mqttStarted;
    static bool schemaPublished;

    static void mqttEventHandler(void* handlerArgs, const char* base, int32_t eventId, void* eventData);
    // serre/cmd (CSV 7 champs) : parseCommand → submitCommand → CommandRouter::route
    static void dispatchCommand(void* eventData);
    static void publishOnline();
    static void publishSchema();
    static String buildSchemaJson();
    static String formatCsvPayload(const DataRecord& record);

    static void (*_onPublishSuccess)();

    // Slot « in-flight » : un record déjà formaté en CSV, en attente de
    // succès d'enqueue esp_mqtt. Si l'enqueue courant échoue, on retente
    // le même payload au tour suivant — aucun record n'est perdu sur
    // erreur transitoire. La backpressure globale (bursts, coupures WiFi)
    // est portée par DataLogger::logBufferOut en amont.
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
};
