// Core/EventManager.h
#pragma once

#include <Arduino.h>

/*
 * EventManager v3.0 — Portage Waveshare ESP32-S3-Relay-6CH
 *
 * Changements par rapport à v2.1 (LilyGo T-SIM7080G-S3) :
 *  - Suppression complète de la section Power (pas de PMU/batterie)
 *
 * Rôle :
 *  - observer les sous-systèmes
 *  - conserver l'état courant ET précédent
 *  - fournir une base saine pour la détection d'événements
 *
 * Toujours :
 *  - aucune règle métier
 *  - aucune action
 *  - aucune persistance
 */

class EventManager {
public:
    static void init();

    // Initialisation explicite des états (appelée une seule fois INIT → RUN)
    static void prime();

    // Appelé périodiquement par TaskManager
    static void handle();

    // ---------------------------------------------------------------------
    // Accès aux états WiFi
    // ---------------------------------------------------------------------

    static bool hasWifiState();
    static bool hasPreviousWifiState();

    static bool isStaEnabled();
    static bool wasStaEnabled();

    static bool isStaConnected();
    static bool wasStaConnected();

    static int  getRssi();
    static int  getPreviousRssi();

private:
    // ---------------------------------------------------------------------
    // États internes
    // ---------------------------------------------------------------------

    struct WifiState {
        bool valid = false;
        bool staEnabled = false;
        bool staConnected = false;
        int  rssi = 0;
    };

    static WifiState  currentWifi;
    static WifiState  previousWifi;

    // Méthodes internes
    static void readWifiState(WifiState& target);
};
