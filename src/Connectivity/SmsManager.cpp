// src/Connectivity/SmsManager.cpp

#include "Connectivity/SmsManager.h"
#include "Connectivity/WiFiManager.h"
#include "Config/NetworkConfig.h"
#include "Config/Config.h"
#include "Utils/Console.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>

// Tag pour logs
static const char* TAG = "SMS";

// =============================================================================
// DEBUG : mettre à true pour désactiver le SMS de démarrage
// =============================================================================
static constexpr bool DEBUG_SKIP_STARTUP_SMS = true;

// =============================================================================
// Membres statiques
// =============================================================================
std::vector<SmsManager::SmsItem> SmsManager::queue;
SmsManager::State SmsManager::currentState = State::IDLE;
int SmsManager::sendRetryCount      = 0;
unsigned long SmsManager::pollStartMs   = 0;
unsigned long SmsManager::lastPollMs    = 0;
unsigned long SmsManager::bootTime      = 0;
bool SmsManager::startupSmsSent         = false;
uint32_t SmsManager::requestIdCounter   = 0;

// =============================================================================
// Initialisation
// =============================================================================
void SmsManager::init()
{
    queue.reserve(MAX_QUEUE_SIZE);
    currentState    = State::IDLE;
    sendRetryCount  = 0;
    bootTime        = millis();
    startupSmsSent  = false;
    requestIdCounter = 0;

    Console::info(TAG, "SmsManager initialisé (HTTP via LilyGo)");
    Console::debug(TAG, "Destinataires configurés: " + String(SMS_NUMBERS_COUNT));
}

// =============================================================================
// Générateur de request_id unique
// =============================================================================
String SmsManager::generateRequestId()
{
    requestIdCounter++;
    return "ws_" + String(requestIdCounter);
}

// =============================================================================
// SMS de bienvenue
// =============================================================================
void SmsManager::sendStartupSms()
{
    String message = "Bonjour, la Serre de Marie-Pierre est bien connectee"
                     " - IP: " + WiFi.localIP().toString() +
                     " - WiFi: " + String(WiFi.RSSI()) + " dBm";

    alert(message);
    startupSmsSent = true;
    Console::info(TAG, "SMS de bienvenue ajouté à la file");
}

// =============================================================================
// Terminer le SMS en cours
// =============================================================================
void SmsManager::finishCurrentSms(bool success, const String& detail)
{
    if (!queue.empty()) {
        if (success) {
            Console::info(TAG, "SMS envoyé à " + queue.front().number +
                         " (id: " + queue.front().requestId + ")");
        } else {
            Console::error(TAG, "SMS échoué pour " + queue.front().number +
                          (detail.length() > 0 ? " — " + detail : ""));
        }
        queue.erase(queue.begin());
    }

    currentState   = State::IDLE;
    sendRetryCount = 0;
}

// =============================================================================
// Handle — Machine d'états (appelée toutes les 2s par TaskManager)
// =============================================================================
void SmsManager::handle()
{
    // Attendre 60s après le boot
    if (millis() - bootTime < STARTUP_DELAY_MS) {
        return;
    }

    // Envoyer SMS de bienvenue (une seule fois)
    if (!startupSmsSent && WiFiManager::isSTAConnected()) {
        if (DEBUG_SKIP_STARTUP_SMS) {
            Console::info(TAG, "DEBUG: SMS de démarrage désactivé");
            startupSmsSent = true;
        } else {
            sendStartupSms();
        }
    }

    // Rien à envoyer
    if (queue.empty()) {
        return;
    }

    // WiFi nécessaire pour joindre la LilyGo
    if (!WiFiManager::isSTAConnected()) {
        return;
    }

    switch (currentState) {

    // -----------------------------------------------------------------
    // IDLE — Démarrage envoi du prochain SMS en file
    // -----------------------------------------------------------------
    case State::IDLE:
        Console::info(TAG, "Début envoi SMS à " + queue.front().number);
        Console::debug(TAG, "Message: " + queue.front().message);
        currentState   = State::HTTP_SEND;
        sendRetryCount = 0;
        break;

    // -----------------------------------------------------------------
    // HTTP_SEND — POST vers la LilyGo
    // -----------------------------------------------------------------
    case State::HTTP_SEND:
        if (httpPostSms(queue.front())) {
            // POST accepté, démarrage du polling
            pollStartMs = millis();
            lastPollMs  = 0;
            currentState = State::POLL_STATUS;
            Console::debug(TAG, "POST accepté, début polling statut");
        } else {
            sendRetryCount++;
            if (sendRetryCount >= MAX_SEND_RETRIES) {
                finishCurrentSms(false, "POST échoué après "
                                 + String(MAX_SEND_RETRIES) + " tentatives");
            } else {
                Console::warn(TAG, "POST échoué, retry "
                             + String(sendRetryCount) + "/" + String(MAX_SEND_RETRIES));
                // Reste en HTTP_SEND, réessaie au prochain handle()
            }
        }
        break;

    // -----------------------------------------------------------------
    // POLL_STATUS — GET /sms/{request_id} jusqu'à résolution
    // -----------------------------------------------------------------
    case State::POLL_STATUS: {
        unsigned long now = millis();

        // Timeout global du polling
        if (now - pollStartMs > POLL_TIMEOUT_MS) {
            finishCurrentSms(false, "Timeout polling ("
                             + String(POLL_TIMEOUT_MS / 1000) + "s)");
            break;
        }

        // Intervalle entre deux polls
        if (now - lastPollMs < POLL_INTERVAL_MS) {
            break;
        }
        lastPollMs = now;

        int result = httpPollStatus(queue.front().requestId);

        switch (result) {
            case 1:    // SENT
                finishCurrentSms(true);
                break;
            case -1:   // FAILED / EXPIRED / NOT_FOUND
                finishCurrentSms(false, "LilyGo rapporte échec");
                break;
            case -2:   // Erreur HTTP
                Console::warn(TAG, "Erreur HTTP polling, on continue...");
                break;
            case 0:    // PENDING / SENDING
                break; // Continue à poller
        }
        break;
    }

    }  // switch
}

// =============================================================================
// POST /sms — Soumettre un SMS à la LilyGo
// =============================================================================
bool SmsManager::httpPostSms(SmsItem& item)
{
    HTTPClient http;
    String url = "http://" + WIFI_STA_GATEWAY.toString() + "/sms";

    http.begin(url);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");

    // Générer le request_id
    item.requestId = generateRequestId();

    // Construire le JSON
    StaticJsonDocument<256> doc;
    doc["to"]         = item.number;
    doc["text"]       = item.message;
    doc["request_id"] = item.requestId;
    doc["source"]     = "waveshare";

    String body;
    serializeJson(doc, body);

    Console::debug(TAG, "POST " + url + " → " + body);

    int httpCode = http.POST(body);
    bool accepted = false;

    if (httpCode == 200) {
        String response = http.getString();
        StaticJsonDocument<256> respDoc;
        DeserializationError err = deserializeJson(respDoc, response);

        if (!err) {
            const char* status = respDoc["status"];
            if (status && (strcmp(status, "ACCEPTED") == 0 ||
                           strcmp(status, "PENDING")  == 0 ||
                           strcmp(status, "SENT")     == 0)) {
                Console::info(TAG, "POST /sms → " + String(status)
                             + " (id: " + item.requestId + ")");
                accepted = true;
            } else {
                const char* reason = respDoc["reason"];
                Console::error(TAG, "POST /sms rejeté: "
                              + String(reason ? reason : "inconnu"));
            }
        } else {
            Console::error(TAG, "POST /sms JSON parse error");
        }
    } else if (httpCode > 0) {
        Console::error(TAG, "POST /sms HTTP " + String(httpCode));
    } else {
        Console::error(TAG, "POST /sms erreur connexion: " + http.errorToString(httpCode));
    }

    http.end();
    return accepted;
}

// =============================================================================
// GET /sms/{request_id} — Consulter le statut d'un SMS
// =============================================================================
int SmsManager::httpPollStatus(const String& requestId)
{
    HTTPClient http;
    String url = "http://" + WIFI_STA_GATEWAY.toString() + "/sms/" + requestId;

    http.begin(url);
    http.setTimeout(HTTP_TIMEOUT_MS);

    int httpCode = http.GET();
    int result = -2;  // Par défaut : erreur HTTP

    if (httpCode == 200) {
        String response = http.getString();
        StaticJsonDocument<256> respDoc;
        DeserializationError err = deserializeJson(respDoc, response);

        if (!err) {
            const char* status = respDoc["status"];
            if (status) {
                Console::debug(TAG, "Poll " + requestId + " → " + String(status));

                if (strcmp(status, "SENT") == 0) {
                    result = 1;
                } else if (strcmp(status, "FAILED")  == 0 ||
                           strcmp(status, "EXPIRED") == 0) {
                    // Log du détail d'erreur si disponible
                    const char* errCode   = respDoc["error_code"];
                    const char* errDetail = respDoc["error_detail"];
                    if (errCode) {
                        Console::warn(TAG, "Détail échec: " + String(errCode)
                                     + (errDetail ? " — " + String(errDetail) : ""));
                    }
                    result = -1;
                } else if (strcmp(status, "PENDING") == 0 ||
                           strcmp(status, "SENDING") == 0) {
                    result = 0;
                }
            }
        } else {
            Console::error(TAG, "GET /sms/ JSON parse error");
        }
    } else if (httpCode == 404) {
        Console::warn(TAG, "Poll " + requestId + " → NOT_FOUND");
        result = -1;
    } else if (httpCode > 0) {
        Console::error(TAG, "GET /sms/ HTTP " + String(httpCode));
    } else {
        Console::error(TAG, "GET /sms/ erreur connexion: " + http.errorToString(httpCode));
    }

    http.end();
    return result;
}

// =============================================================================
// Alert — Envoie un message à tous les numéros configurés
// =============================================================================
void SmsManager::alert(const String& message)
{
    for (size_t i = 0; i < SMS_NUMBERS_COUNT; i++) {
        send(SMS_NUMBERS[i], message);
    }
}

// =============================================================================
// Send — Ajoute un SMS à la file d'attente
// =============================================================================
void SmsManager::send(const char* number, const String& message)
{
    if (queue.size() >= MAX_QUEUE_SIZE) {
        Console::warn(TAG, "File pleine, suppression du plus ancien");
        queue.erase(queue.begin());
    }

    SmsItem item;
    item.number  = String(number);
    item.message = message;
    // requestId sera attribué au moment du POST
    queue.push_back(item);

    Console::debug(TAG, "SMS en file pour " + item.number
                  + " (" + String(queue.size()) + " en attente)");
}

// =============================================================================
// QueueSize — Nombre de SMS en attente
// =============================================================================
size_t SmsManager::queueSize()
{
    return queue.size();
}

// =============================================================================
// IsBusy — Envoi en cours
// =============================================================================
bool SmsManager::isBusy()
{
    return currentState != State::IDLE;
}