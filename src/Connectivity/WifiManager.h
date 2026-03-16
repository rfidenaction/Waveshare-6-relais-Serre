// src/Connectivity/WiFiManager.h
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include "Config/NetworkConfig.h"

/*
 * WiFiManager — Machine d'états non-bloquante
 *
 * Politique WiFi (FIGÉE) :
 *
 * - AP + STA TOUJOURS démarrés au boot
 * - AP peut être coupé à chaud (irréversible sans reboot)
 * - STA toujours actif (connexion vers LilyGo pont WiFi-GSM)
 *
 * États autorisés :
 * - AP ON  + STA ON   (défaut au boot)
 * - AP OFF + STA ON   (après disableAP())
 *
 * Intégration TaskManager :
 * - handle() appelé toutes les 250ms par TaskManager
 * - Chaque appel traite UN état puis retourne
 * - Budget temps garanti < 15ms sauf AP_START unique (~725ms)
 *
 * Priorité absolue : STABILITÉ
 * Le WiFi n'est pas critique — l'arrosage et les capteurs priment.
 */
class WiFiManager {
public:
    // -------------------------------------------------------------------------
    // Initialisation et gestion
    // -------------------------------------------------------------------------
    static void init();     // Active radio WiFi + lwIP (mode AP_STA)
    static void handle();   // Machine d'états, appelée par TaskManager

    // -------------------------------------------------------------------------
    // Contrôle (via flag, appliqué en état stable)
    // -------------------------------------------------------------------------
    static void disableAP();    // Demande coupure AP à chaud (irréversible)

    // -------------------------------------------------------------------------
    // États (lecture seule)
    // -------------------------------------------------------------------------
    static bool isSTAConnected();
    static bool isAPEnabled();

    // -------------------------------------------------------------------------
    // Infos pour interface Web
    // -------------------------------------------------------------------------
    static String getSTAStatus();
    static String getAPStatus();

private:
    // -------------------------------------------------------------------------
    // Machine d'états
    // -------------------------------------------------------------------------

    /*
     * ZONE BOOT (traversée une seule fois, jamais revisitée) :
     *
     *   [init() appelle WiFi.mode(WIFI_AP_STA) — prérequis lwIP — ~50ms]
     *
     *   AP_CONFIG ────── WiFi.softAPConfig()  ~1ms
     *        │
     *        ▼
     *   AP_START ─────── WiFi.softAP()        ~0.1ms ou ~725ms (UNIQUE)
     *        │
     *        ▼
     *   AP_STABILIZE ── attente 5s (stabilisation driver AP+STA)
     *        │
     *        ▼
     *   STA_CONFIG ───── WiFi.config()        ~4ms
     *        │
     *        ▼
     *   STA_BEGIN ────── WiFi.begin()         ~2ms
     *        │
     *        ▼
     *   STA_CONNECTING
     *
     * ZONE RÉGIME PERMANENT (boucle bornée) :
     *
     *   STA_CONNECTING ─ timeout 30s ──► STA_DISCONNECT
     *        │
     *        └── WL_CONNECTED ──► STA_CONNECTED
     *
     *   STA_CONNECTED ── perte connexion ──► STA_DISCONNECT
     *
     *   STA_DISCONNECT ─ WiFi.disconnect() ──► STA_WAIT_RETRY
     *
     *   STA_WAIT_RETRY ─ attente 30s (×2) puis 300s ──► STA_CONFIG (reboucle)
     *
     * Aucun chemin ne remonte vers la zone boot.
     * La demande externe (disableAP) est appliquée
     * uniquement dans les états stables : STA_CONNECTED, STA_WAIT_RETRY.
     */
    enum class State {
        // Zone boot (traversée une seule fois)
        AP_CONFIG,
        AP_START,
        AP_STABILIZE,
        STA_CONFIG,
        STA_BEGIN,
        // Zone régime permanent
        STA_CONNECTING,
        STA_CONNECTED,
        STA_DISCONNECT,
        STA_WAIT_RETRY
    };

    static State state;

    // -------------------------------------------------------------------------
    // Méthodes internes
    // -------------------------------------------------------------------------
    static void applyPendingRequests();                        // Applique flag dans états stables
    static void changeState(State newState);                   // Transition + log
    static const char* wlStatusToString(wl_status_t status);   // Traduction lisible

    // -------------------------------------------------------------------------
    // États runtime
    // -------------------------------------------------------------------------
    static bool staConnected;   // Connexion STA effective
    static bool apEnabled;      // AP actif
    static uint8_t staRetryCount;  // Compteur de tentatives STA échouées

    // -------------------------------------------------------------------------
    // Timing
    // -------------------------------------------------------------------------
    static unsigned long apStabilizeStartMs;
    static unsigned long connectStartMs;
    static unsigned long retryStartMs;
    static unsigned long lastConnectLogMs;

    // -------------------------------------------------------------------------
    // Constantes internes
    // -------------------------------------------------------------------------
    static constexpr unsigned long AP_STABILIZE_MS         = 5000;    // Stabilisation post-AP (ESP32-S3 AP+STA a besoin de temps)
    static constexpr unsigned long STA_CONNECT_TIMEOUT_MS  = 30000;   // Timeout connexion
    static constexpr unsigned long STA_CONNECT_LOG_MS      = 5000;    // Log périodique
    static constexpr unsigned long STA_EARLY_FAIL_GRACE_MS = 15000;    // Grâce avant détection échec rapide

    // Backoff STA : 2 essais agressifs (30s), puis passage à 300s
    // Protège l'AP quand le réseau STA est absent longtemps
    static constexpr unsigned long STA_RETRY_DELAY_MS      = 30000;   // Délai retries agressifs
    static constexpr unsigned long STA_SLOW_RETRY_DELAY_MS = 300000;  // Délai retries espacés (5min)
    static constexpr uint8_t      STA_AGGRESSIVE_RETRIES   = 3;       // Nombre d'essais avant backoff

    // -------------------------------------------------------------------------
    // Flag de demande externe (concurrence web → machine d'états)
    // -------------------------------------------------------------------------
    static bool apDisableRequested;
};