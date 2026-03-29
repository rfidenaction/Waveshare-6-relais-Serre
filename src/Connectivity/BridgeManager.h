// src/Connectivity/BridgeManager.h
#pragma once

#include <Arduino.h>
#include <WiFiUdp.h>

/*
 * BridgeManager — Point unique de communication Waveshare ↔ LilyGo
 *
 * Transport : UDP unicast bidirectionnel
 *   Waveshare (192.168.4.10:5001) ↔ LilyGo (192.168.4.1:5000)
 *
 * Flux entrant (LilyGo → Waveshare) :
 *   STATE|0 ou STATE|1  — disponibilité SMS (~30s)
 *   ACK                 — confirmation SMS envoyé par le modem
 *
 * Flux sortant (Waveshare → LilyGo) :
 *   HB                  — heartbeat après 5 publications MQTT
 *   SMS|number|text      — ordre d'envoi de SMS
 *
 * Démarrage différé :
 *   - init() initialise les variables mais n'ouvre PAS le socket UDP
 *   - handle() ne fait rien pendant BRIDGE_START_DELAY_MS (4 minutes)
 *   - Au premier appel après le délai : ouverture du socket UDP
 *   - Ceci laisse le temps au WiFi STA, MQTT et NTP de se stabiliser
 *
 * Principe :
 *   - handle() appelé par TaskManager (~500ms)
 *   - Chaque appel : recvfrom non-bloquant + machine d'états SMS
 *   - Aucun appel réseau bloquant, jamais
 *   - canAcceptSms est mis à jour passivement par les paquets STATE
 *
 * Heartbeat :
 *   - MqttManager notifie BridgeManager à chaque publication réussie
 *     via le callback onMqttPublish()
 *   - BridgeManager compte et envoie un heartbeat UDP toutes les 5 publications
 *   - Le heartbeat signifie "la Waveshare est vivante ET produit des données"
 *
 * Cycle SMS :
 *   1. SMS en queue + canAcceptSms → envoi UDP
 *   2. Attente ACK (3 min max, via recvfrom non-bloquant)
 *   3. ACK reçu → succès | timeout → retry 1 fois | abandon
 *   4. Chaque événement SMS est loggé Console + DataLogger (→ MQTT auto)
 *
 * Intégration :
 *   - init() dans loopInit() après WiFiManager
 *   - handle() enregistré comme tâche TaskManager
 *   - MqttManager::setOnPublishSuccess(BridgeManager::onMqttPublish) dans main
 *   - queueSms() appelé par SmsManager (logique métier)
 *   - SmsManager et MqttManager ne communiquent JAMAIS directement
 *     avec la LilyGo — tout passe par BridgeManager
 */
class BridgeManager {
public:
    // ─── Cycle de vie ────────────────────────────────────────────────────
    static void init();         // Initialise les variables (pas de socket)
    static void handle();       // Appelé par TaskManager (~500ms)

    // ─── Heartbeat (callback appelé par MqttManager après chaque pub) ────
    static void onMqttPublish();    // Compte et envoie HB toutes les 5 pubs

    // ─── SMS (appelé par SmsManager) ─────────────────────────────────────
    // Retourne false si queue pleine (2 max) — le SMS est rejeté + loggé
    static bool queueSms(const String& number, const String& message);

    // ─── Accesseur (lecture instantanée, non-bloquant) ───────────────────
    static bool canAcceptSms();

private:
    // ─── Slot SMS ────────────────────────────────────────────────────────
    struct SmsSlot {
        String   number;            // Numéro destinataire (format E.164)
        String   message;           // Texte du SMS (max 160 car ASCII)
    };

    // ─── Machine d'états SMS ─────────────────────────────────────────────
    enum class SmsState : uint8_t {
        IDLE,                       // Pas d'envoi en cours
        WAIT_ACK                    // SMS envoyé, attente ACK de la LilyGo
    };

    // ─── Socket UDP (un seul, bidirectionnel) ────────────────────────────
    static WiFiUDP udp;
    static bool    started;         // true après udp.begin() (démarrage différé)

    // ─── Timing démarrage ────────────────────────────────────────────────
    static uint32_t bootTime;       // millis() au moment de init()

    // ─── État LilyGo (un seul booléen, mis à jour par STATE) ─────────────
    static bool _canAcceptSms;

    // ─── Queue SMS (tableau fixe, 2 slots max) ───────────────────────────
    static constexpr uint8_t SMS_QUEUE_SIZE = 2;
    static SmsSlot  smsQueue[SMS_QUEUE_SIZE];
    static uint8_t  smsCount;               // Nombre de SMS en queue (0, 1 ou 2)

    // ─── État machine SMS ────────────────────────────────────────────────
    static SmsState  smsState;
    static uint8_t   smsAttempt;            // 1 ou 2 (max 2 tentatives)
    static uint32_t  smsSentMs;             // millis() de la dernière tentative

    // ─── Flag ACK (positionné par processIncoming, lu par handleSmsMachine) ──
    static bool      ackReceived;

    // ─── Compteur heartbeat ──────────────────────────────────────────────
    static uint8_t   publishCounter;        // Publications MQTT réussies depuis dernier HB

    // ─── Méthodes internes ───────────────────────────────────────────────
    static void processIncoming();                          // recvfrom non-bloquant, dispatch STATE/ACK
    static void handleSmsMachine();                         // Machine d'états SMS
    static void sendSmsPacket(const SmsSlot& sms);          // Envoie SMS|number|text en UDP
    static void sendHeartbeat();                            // Envoie HB en UDP
    static void removeFrontSms();                           // Supprime le premier SMS de la queue
    static void logSmsEvent(const String& message);         // Console + DataLogger::push(Error)
};