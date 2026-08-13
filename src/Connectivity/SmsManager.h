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

/*
 * Plage de période au-delà de laquelle une dérive du scheduler arme un SMS.
 *
 * Ces deux bornes ne conditionnent QUE l'alerte SMS. Elles n'ont aucun effet
 * sur la valeur publiée sur DataId::TaskMonPeriod, qui remonte toujours la
 * pire période réellement mesurée sur l'heure écoulée, ni sur la trace console
 * de TaskManagerMonitor, dont le seuil est écrit en dur.
 *
 * Les élargir réduit le nombre de SMS sans jamais dégrader l'affichage.
 */
#define SMS_TASKMON_MIN_PERIOD_MS       1500
#define SMS_TASKMON_MAX_PERIOD_MS       2500

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