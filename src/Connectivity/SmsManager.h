// src/Connectivity/SmsManager.h
#pragma once
#include <Arduino.h>

// =============================================================================
// POLITIQUE D'ALERTES SMS — ACTIVATION / DESACTIVATION / REGLAGES
// =============================================================================
// Tout ce qui concerne le déclenchement des SMS se règle ici.
// Pour désactiver un type d'alerte : mettre le flag correspondant à false.
// Pour couper TOUS les SMS (debug terrain) : mettre SMS_GLOBALLY_ENABLED à false.
// =============================================================================

/*
 * Interrupteur global : bloque TOUT envoi de SMS quand il est à false.
 * Utile pendant un debug terrain pour éviter de spammer les destinataires.
 * Lorsque ce flag est à false, toute tentative d'envoi est loggée en Console
 * mais aucun SMS ne part et aucun DataLog n'est produit.
 */
#define SMS_GLOBALLY_ENABLED            false

// -----------------------------------------------------------------------------
// Alerte : SMS de démarrage (boot réussi)
// -----------------------------------------------------------------------------
#define SMS_BOOT_ENABLED                true
#define SMS_BOOT_DELAY_MS               60000UL      // 60 s après boot

// -----------------------------------------------------------------------------
// Alerte : dérive du scheduler (TaskManagerMonitor)
// -----------------------------------------------------------------------------
#define SMS_TASKMON_ENABLED             true
#define SMS_TASKMON_GRACE_MS            180000UL     // 3 min après init monitor
#define SMS_TASKMON_COOLDOWN_MS         172800000UL  // 48 h entre deux SMS
#define SMS_TASKMON_BEFORE_SENDING_MS   60000UL      // 1 min entre armement et envoi

// =============================================================================
// (fin de la section politique d'alertes)
// =============================================================================


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
 * - Politique d'alertes centralisée en tête de ce fichier
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
    // État
    // -------------------------------------------------------------------------
    static unsigned long bootTime;
    static bool startupSmsSent;

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------
    static void sendStartupSms();
};