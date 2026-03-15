// src/Connectivity/SmsManager.h
#pragma once
#include <Arduino.h>
#include <vector>

/*
 * SmsManager — Envoi de SMS via la LilyGo (pont WiFi-GSM)
 *
 * Principe :
 * - La Waveshare n'a pas de modem, les SMS passent par la LilyGo
 * - POST http://<gateway>/sms         → soumettre un SMS
 * - GET  http://<gateway>/sms/{id}    → suivre le statut
 *
 * Machine d'états non-bloquante (compatible TaskManager) :
 * - IDLE       → vérifie si SMS en file
 * - HTTP_SEND  → POST vers LilyGo
 * - POLL_STATUS → GET statut jusqu'à SENT, FAILED ou timeout
 *
 * Contraintes LilyGo :
 * - Rate limit : 5 min entre deux envois réussis (géré côté LilyGo)
 * - Queue LilyGo : 10 slots max
 * - Texte : max 160 caractères ASCII (pas d'accents)
 * - Numéro : format E.164, whitelisté côté LilyGo
 */

class SmsManager {
public:
    // Cycle de vie
    static void init();
    static void handle();   // Appelé par TaskManager toutes les 2s

    // Envoi de SMS
    static void alert(const String& message);                    // Envoie à tous les numéros configurés
    static void send(const char* number, const String& message); // Envoi à un numéro spécifique

    // Monitoring
    static size_t queueSize();
    static bool isBusy();

private:
    // Structure d'un SMS en attente
    struct SmsItem {
        String number;
        String message;
        String requestId;   // Attribué au moment du POST
    };

    // Machine d'états
    enum class State {
        IDLE,
        HTTP_SEND,
        POLL_STATUS
    };

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------
    static constexpr size_t        MAX_QUEUE_SIZE      = 10;
    static constexpr int           MAX_SEND_RETRIES    = 3;
    static constexpr unsigned long HTTP_TIMEOUT_MS     = 5000;   // Timeout HTTP par requête
    static constexpr unsigned long POLL_INTERVAL_MS    = 5000;   // Intervalle entre polls
    static constexpr unsigned long POLL_TIMEOUT_MS     = 60000;  // Timeout global du polling
    static constexpr unsigned long STARTUP_DELAY_MS    = 60000;  // 60s après boot

    // -------------------------------------------------------------------------
    // File d'attente
    // -------------------------------------------------------------------------
    static std::vector<SmsItem> queue;

    // -------------------------------------------------------------------------
    // Machine d'états
    // -------------------------------------------------------------------------
    static State currentState;
    static int sendRetryCount;
    static unsigned long pollStartMs;
    static unsigned long lastPollMs;
    static unsigned long bootTime;
    static bool startupSmsSent;

    // -------------------------------------------------------------------------
    // Générateur de request_id
    // -------------------------------------------------------------------------
    static uint32_t requestIdCounter;
    static String generateRequestId();

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------
    static void finishCurrentSms(bool success, const String& detail = "");
    static void sendStartupSms();

    // Requêtes HTTP vers la LilyGo
    static bool httpPostSms(SmsItem& item);     // true = accepté par LilyGo
    static int  httpPollStatus(const String& requestId);
    //   Retourne :  1 = SENT
    //               0 = PENDING / SENDING (continuer à poller)
    //              -1 = FAILED / EXPIRED / NOT_FOUND
    //              -2 = erreur HTTP (continuer à poller)
};