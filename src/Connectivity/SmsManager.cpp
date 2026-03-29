// src/Connectivity/SmsManager.cpp
//
// Logique métier SMS : décide QUAND et QUOI envoyer.
// Le transport (UDP vers LilyGo) est entièrement délégué à BridgeManager.

#include "Connectivity/SmsManager.h"
#include "Connectivity/BridgeManager.h"
#include "Connectivity/WiFiManager.h"
#include "Config/NetworkConfig.h"
#include "Config/Config.h"
#include "Utils/Console.h"

// Tag pour logs
static const char* TAG = "SMS";

// =============================================================================
// DEBUG : mettre à true pour désactiver le SMS de démarrage
// ⚠️ VARIABLE DE CONTRÔLE — basculer à false pour la production
// =============================================================================
static constexpr bool DEBUG_SKIP_STARTUP_SMS = true;

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

    Console::info(TAG, "SmsManager initialisé (transport via BridgeManager)");
    Console::debug(TAG, "Destinataires configurés: " + String(SMS_NUMBERS_COUNT));
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
    Console::info(TAG, "SMS de bienvenue envoyé à BridgeManager");
}

// =============================================================================
// Handle — Gestion du SMS de démarrage (appelé par TaskManager)
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
// =============================================================================
void SmsManager::send(const char* number, const String& message)
{
    bool accepted = BridgeManager::queueSms(String(number), message);

    if (!accepted) {
        Console::warn(TAG, "SMS rejeté par BridgeManager (queue pleine) dest:" + String(number));
    }
}