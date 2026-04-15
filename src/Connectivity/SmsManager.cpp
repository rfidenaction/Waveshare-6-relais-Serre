// src/Connectivity/SmsManager.cpp
//
// Logique métier SMS : décide QUAND et QUOI envoyer.
// Le transport (UDP vers LilyGo) est entièrement délégué à BridgeManager.
//
// Politique d'alertes (activation, timings) : voir en tête de SmsManager.h

#include "Connectivity/SmsManager.h"
#include "Connectivity/BridgeManager.h"
#include "Connectivity/WiFiManager.h"
#include "Config/NetworkConfig.h"
#include "Config/Config.h"
#include "Storage/DataLogger.h"
#include "Utils/Console.h"

// Tag pour logs
static const char* TAG = "SMS";

// =============================================================================
// Membres statiques
// =============================================================================
unsigned long SmsManager::bootTime       = 0;
bool          SmsManager::startupSmsSent = false;

// =============================================================================
// Initialisation
// =============================================================================
void SmsManager::init()
{
    bootTime        = millis();
    startupSmsSent  = false;

    Console::info(TAG, "SmsManager initialise (transport via BridgeManager)");
    Console::debug(TAG, "Destinataires configures: " + String(SMS_NUMBERS_COUNT));
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
    Console::info(TAG, "SMS de bienvenue transmis a BridgeManager");
}

// =============================================================================
// Handle — Gestion du SMS de démarrage (appelé par TaskManager)
// =============================================================================
void SmsManager::handle()
{
    // Attendre le délai configuré après le boot
    if (millis() - bootTime < SMS_BOOT_DELAY_MS) {
        return;
    }

    // Envoyer SMS de bienvenue (une seule fois)
    if (!startupSmsSent && WiFiManager::isSTAConnected()) {
        if (!SMS_BOOT_ENABLED) {
            Console::info(TAG, "SMS de boot desactive (SMS_BOOT_ENABLED=false)");
            startupSmsSent = true;
        } else {
            sendStartupSms();
        }
    }
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
// Send — Délègue le transport à BridgeManager
//
// Comportement :
//  1. Si SMS_GLOBALLY_ENABLED == false : log Console + return, aucun envoi
//  2. Sinon : log Console + DataLog (trace persistante de la tentative)
//     puis appel à BridgeManager::queueSms() pour le transport effectif.
//     Les évenements ultérieurs (mise en file, ACK, abandon...) sont
//     également loggés par BridgeManager dans le meme canal DataId::SmsEvent.
// =============================================================================
void SmsManager::send(const char* number, const String& message)
{
    // ─── Filtre global : SMS_GLOBALLY_ENABLED ───────────────────────────────
    if (!SMS_GLOBALLY_ENABLED) {
        Console::info(TAG,
            "SMS bloque (SMS_GLOBALLY_ENABLED=false) dest:" + String(number)
            + " msg:" + message);
        return;
    }

    // ─── Trace de la tentative d'envoi (Console + DataLog) ──────────────────
    Console::info(TAG,
        "Demande envoi SMS dest:" + String(number) + " msg:" + message);

    DataLogger::push(
        DataId::SmsEvent,
        "Demande envoi vers " + String(number)
    );

    // ─── Délégation au transport ────────────────────────────────────────────
    bool accepted = BridgeManager::queueSms(String(number), message);

    if (!accepted) {
        Console::info(TAG,
            "SMS rejete par BridgeManager (queue pleine) dest:" + String(number));
    }
}