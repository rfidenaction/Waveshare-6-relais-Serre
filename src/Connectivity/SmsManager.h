// src/Connectivity/SmsManager.h
#pragma once
#include <Arduino.h>

/*
 * SmsManager — Logique métier SMS (quand envoyer, à qui, quel message)
 *
 * Principe :
 * - SmsManager décide QUAND et QUOI envoyer
 * - BridgeManager gère le COMMENT (transport UDP vers LilyGo)
 * - SmsManager ne fait AUCUN appel réseau
 *
 * Fonctionnalités :
 * - SMS de bienvenue au boot (une fois, après stabilisation WiFi STA)
 * - Envoi d'alertes à tous les numéros configurés
 * - Flag DEBUG pour désactiver le SMS de démarrage pendant le développement
 *
 * Intégration :
 * - init() appelé dans loopInit()
 * - handle() appelé par TaskManager (~2s)
 * - Appelle BridgeManager::queueSms() pour le transport
 */

class SmsManager {
public:
    // Cycle de vie
    static void init();
    static void handle();   // Appelé par TaskManager

    // Envoi de SMS
    static void alert(const String& message);                    // Envoie à tous les numéros configurés
    static void send(const char* number, const String& message); // Envoi à un numéro spécifique

private:
    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------
    static constexpr unsigned long STARTUP_DELAY_MS = 60000;  // 60s après boot

    // -------------------------------------------------------------------------
    // État
    // -------------------------------------------------------------------------
    static unsigned long bootTime;
    static bool startupSmsSent;

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------
    static void sendStartupSms();
};