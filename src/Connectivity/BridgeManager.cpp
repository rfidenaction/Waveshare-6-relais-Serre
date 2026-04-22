// src/Connectivity/BridgeManager.cpp
//
// Point unique de communication Waveshare ↔ LilyGo via UDP unicast.
//
// Démarrage différé : le socket UDP n'est ouvert qu'après
// BRIDGE_START_DELAY_MS (4 minutes), pour laisser WiFi STA, MQTT
// et NTP se stabiliser sans encombrement réseau.
//
// Protocole UDP (5 types de paquets, sans identifiant) :
//   Waveshare → LilyGo :
//     HB                    — heartbeat (après 5 publications MQTT)
//     SMS|number|text        — ordre d'envoi de SMS
//     MqttKo                — MQTT KO prolonge, demande renegociation PPP
//   LilyGo → Waveshare :
//     STATE|0 ou STATE|1    — disponibilité SMS (~30s)
//     ACK                   — SMS envoyé par le modem
//
// Architecture heartbeat cross-thread :
//   - onMqttPublish() est appele depuis le thread esp_mqtt (via MQTT_EVENT_PUBLISHED).
//     Il se contente d'incrementer publishCounter et, toutes les 5 publications,
//     de lever le flag _heartbeatPending. AUCUN acces UDP dans ce thread.
//   - handle() (thread TaskManager) consulte _heartbeatPending a chaque tour et,
//     si leve, envoie le paquet HB en UDP puis efface le flag.
//   - Cette separation garantit que WiFiUDP (non thread-safe) n'est accede que
//     par un seul thread (TaskManager). Elle conserve la semantique forte du
//     heartbeat : il n'est envoye que lorsqu'une publication MQTT a ete
//     effectivement transmise sur la socket (MQTT_EVENT_PUBLISHED).

#include "Connectivity/BridgeManager.h"
#include "Connectivity/WiFiManager.h"
#include "Config/NetworkConfig.h"
#include "Config/TimingConfig.h"
#include "Storage/DataLogger.h"
#include "Utils/Console.h"

// Tag pour logs Console
static const char* TAG = "BRIDGE";

// =============================================================================
// Variables statiques
// =============================================================================
WiFiUDP              BridgeManager::udp;
bool                 BridgeManager::started              = false;
uint32_t             BridgeManager::bootTime             = 0;
bool                 BridgeManager::_canAcceptSms        = false;
BridgeManager::SmsSlot     BridgeManager::smsQueue[SMS_QUEUE_SIZE];
uint8_t              BridgeManager::smsCount             = 0;
BridgeManager::SmsState    BridgeManager::smsState       = SmsState::IDLE;
uint8_t              BridgeManager::smsAttempt            = 0;
uint32_t             BridgeManager::smsSentMs             = 0;
bool                 BridgeManager::ackReceived           = false;
uint8_t              BridgeManager::publishCounter        = 0;
volatile bool        BridgeManager::_heartbeatPending     = false;

// =============================================================================
// Initialisation — pas de socket UDP ici
// =============================================================================
void BridgeManager::init()
{
    bootTime = millis();
    started  = false;

    _canAcceptSms   = false;

    // Queue SMS vide
    smsCount        = 0;
    smsState        = SmsState::IDLE;
    ackReceived     = false;
    publishCounter  = 0;

    // Flag heartbeat cross-thread
    _heartbeatPending = false;

    Console::info(TAG, "Initialise (demarrage differe "
                      + String(BRIDGE_START_DELAY_MS / 60000) + " minutes)");
}

// =============================================================================
// Handle — appelé par TaskManager (non-bloquant, ~quelques µs)
// =============================================================================
void BridgeManager::handle()
{
    // ─── Attente démarrage différé ───────────────────────────────────────
    if (!started) {
        if (millis() - bootTime < BRIDGE_START_DELAY_MS) return;

        // Premier appel après le délai : ouverture du socket UDP
        udp.begin(BRIDGE_UDP_PORT_LOCAL);
        started = true;

        Console::info(TAG, "Demarre — ecoute UDP port " + String(BRIDGE_UDP_PORT_LOCAL)
                          + ", envoi vers LilyGo port " + String(BRIDGE_UDP_PORT_REMOTE));
        return;
    }

    // ─── Heartbeat differe (leve par onMqttPublish dans thread esp_mqtt) ─
    if (_heartbeatPending) {
        _heartbeatPending = false;
        sendHeartbeat();
    }

    // ─── Fonctionnement normal ───────────────────────────────────────────
    processIncoming();
    handleSmsMachine();
}

// =============================================================================
// Réception UDP non-bloquante
// Draine TOUS les paquets en attente, dispatch selon le type.
// parsePacket() retourne 0 immédiatement s'il n'y a rien.
// =============================================================================
void BridgeManager::processIncoming()
{
    while (true) {
        int packetSize = udp.parsePacket();
        if (packetSize <= 0) break;

        char buf[256];
        int len = udp.read(buf, sizeof(buf) - 1);
        if (len <= 0) continue;
        buf[len] = '\0';

        // ── STATE|0 ou STATE|1 ───────────────────────────────────────────
        if (len >= 7 && strncmp(buf, "STATE|", 6) == 0) {
            _canAcceptSms = (buf[6] == '1');

            Console::info(TAG, "Etat LilyGo recu — SMS:"
                              + String(_canAcceptSms ? "disponible" : "indisponible"));
        }
        // ── ACK ──────────────────────────────────────────────────────────
        else if (len == 3 && strncmp(buf, "ACK", 3) == 0) {
            Console::info(TAG, "ACK recu — SMS confirme par LilyGo");

            if (smsState == SmsState::WAIT_ACK) {
                ackReceived = true;
            }
        }
        // ── Paquet inconnu ───────────────────────────────────────────────
        else {
            Console::warn(TAG, "Paquet UDP inconnu recu: " + String(buf));
        }
    }
}

// =============================================================================
// Machine d'états SMS
//
// IDLE
//   └─ SMS en queue + canAcceptSms → envoie → WAIT_ACK
//
// WAIT_ACK
//   ├─ ACK reçu → succès, supprime SMS → IDLE
//   └─ Timeout 3 min :
//        ├─ attempt==1 + canAcceptSms → retry → WAIT_ACK
//        ├─ attempt==1 + !canAcceptSms → abandon → IDLE
//        └─ attempt==2 → abandon → IDLE
// =============================================================================
void BridgeManager::handleSmsMachine()
{
    switch (smsState) {

    // ─── IDLE — vérifier si un SMS attend ────────────────────────────────
    case SmsState::IDLE:
        if (smsCount == 0)       return;    // Rien à envoyer
        if (!_canAcceptSms)      return;    // LilyGo pas prête

        // Envoyer le premier SMS de la queue
        smsAttempt      = 1;
        ackReceived     = false;
        smsSentMs       = millis();

        sendSmsPacket(smsQueue[0]);
        smsState = SmsState::WAIT_ACK;

        logSmsEvent("SMS envoye tentative 1 vers " + smsQueue[0].number);
        break;

    // ─── WAIT_ACK — attente non-bloquante de la confirmation ─────────────
    case SmsState::WAIT_ACK:
        // ACK reçu → succès
        if (ackReceived) {
            logSmsEvent("SMS confirme par LilyGo vers " + smsQueue[0].number);
            removeFrontSms();
            smsState = SmsState::IDLE;
            return;
        }

        // Pas encore timeout → on attend
        if (millis() - smsSentMs < BRIDGE_SMS_ACK_TIMEOUT_MS) return;

        // ── Timeout 3 minutes atteint ────────────────────────────────────

        if (smsAttempt < 2 && _canAcceptSms) {
            // Deuxième tentative
            smsAttempt  = 2;
            ackReceived = false;
            smsSentMs   = millis();

            sendSmsPacket(smsQueue[0]);

            logSmsEvent("SMS renvoye tentative 2 vers " + smsQueue[0].number);
        } else {
            // Abandon définitif
            String reason = (smsAttempt >= 2) ? "2 tentatives sans reponse"
                                              : "LilyGo non disponible pour SMS";

            logSmsEvent("SMS abandonne vers " + smsQueue[0].number
                       + " — " + reason);
            removeFrontSms();
            smsState = SmsState::IDLE;
        }
        break;
    }
}

// =============================================================================
// Envoi UDP : SMS|number|text
// =============================================================================
void BridgeManager::sendSmsPacket(const SmsSlot& sms)
{
    String packet = "SMS|" + sms.number + "|" + sms.message;

    udp.beginPacket(WIFI_STA_GATEWAY, BRIDGE_UDP_PORT_REMOTE);
    udp.write((const uint8_t*)packet.c_str(), packet.length());
    udp.endPacket();

    Console::info(TAG, "Ordre SMS transmis a LilyGo — dest:" + sms.number);
}

// =============================================================================
// Callback MqttManager → compteur publications réussies → heartbeat differe
//
// ATTENTION : appele depuis le thread esp_mqtt (via MQTT_EVENT_PUBLISHED).
// Ne fait qu'incrementer un compteur et lever un flag. AUCUN acces UDP ici :
// WiFiUDP n'est pas thread-safe, et l'envoi reel est differe vers handle()
// qui tourne dans le thread TaskManager.
//
// Compteur modulo 5 : le heartbeat signifie "la Waveshare est vivante ET
// produit des donnees MQTT effectivement transmises au broker".
// =============================================================================
void BridgeManager::onMqttPublish()
{
    if (!started) return;

    publishCounter++;
    if (publishCounter >= 5) {
        _heartbeatPending = true;
        publishCounter = 0;
    }
}

// =============================================================================
// Heartbeat — envoi UDP fire-and-forget
// Appele uniquement depuis handle() (thread TaskManager).
// =============================================================================
void BridgeManager::sendHeartbeat()
{
    if (!WiFiManager::isSTAConnected()) return;

    udp.beginPacket(WIFI_STA_GATEWAY, BRIDGE_UDP_PORT_REMOTE);
    udp.write((const uint8_t*)"HB", 2);
    udp.endPacket();

    Console::info(TAG, "Heartbeat envoye (signe de vie)");
}

// =============================================================================
// MqttKo — envoi UDP fire-and-forget vers LilyGo
//
// Declenche cote LilyGo une renegociation PPP immediate (DATA→COMMAND→DATA
// + enable_napt()) pour resynchroniser le NAPT / DNS. Appele par MqttManager
// quand la connexion MQTT reste KO au-dela de MQTT_KO_FIRST_DELAY_MS, puis
// toutes les MQTT_KO_REPEAT_DELAY_MS tant que MQTT reste KO.
// =============================================================================
void BridgeManager::sendMqttKo()
{
    if (!started) return;
    if (!WiFiManager::isSTAConnected()) return;

    udp.beginPacket(WIFI_STA_GATEWAY, BRIDGE_UDP_PORT_REMOTE);
    udp.write((const uint8_t*)"MqttKo", 6);
    udp.endPacket();

    Console::warn(TAG, "MqttKo envoye a LilyGo — demande renegociation PPP");
}

// =============================================================================
// Queue SMS — ajouter un SMS (appelé par SmsManager)
// Retourne false si la queue est pleine (le SMS est rejeté)
// =============================================================================
bool BridgeManager::queueSms(const String& number, const String& message)
{
    if (smsCount >= SMS_QUEUE_SIZE) {
        logSmsEvent("SMS rejete (file pleine) vers " + number);
        return false;
    }

    smsQueue[smsCount].number  = number;
    smsQueue[smsCount].message = message;
    smsCount++;

    logSmsEvent("SMS mis en file vers " + number
               + " (" + String(smsCount) + "/" + String(SMS_QUEUE_SIZE) + ")");
    return true;
}

// =============================================================================
// Supprime le premier SMS de la queue (décalage)
// =============================================================================
void BridgeManager::removeFrontSms()
{
    if (smsCount == 0) return;

    // Décaler : le slot 1 passe en slot 0
    if (smsCount > 1) {
        smsQueue[0] = smsQueue[1];
    }
    smsCount--;
}

// =============================================================================
// Log SMS : Console série + DataLogger (→ publication MQTT automatique)
//
// Tous les événements du cycle de vie d'un SMS (mise en file, tentatives,
// ACK, abandon) sont tracés sur le canal dédié DataId::SmsEvent pour
// centraliser la visibilité cote telephone/MQTT.
// =============================================================================
void BridgeManager::logSmsEvent(const String& message)
{
    Console::info(TAG, message);
    DataLogger::push(DataId::SmsEvent, message);
}

// =============================================================================
// Accesseur — lecture instantanée, aucun appel réseau
// =============================================================================
bool BridgeManager::canAcceptSms() { return _canAcceptSms; }