// Web/WebServer.h
// Portage Waveshare ESP32-S3-Relay-6CH
// Changements :
//  - Suppression handleWifiToggle (STA toujours actif)
//  - Suppression handleGsmToggle (pas de modem cellulaire)
//  - Suppression handleGraphData : les graphiques sont servis via /logs/download
//    (bundle complet), le filtrage se fait côté client
#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>

class WebServer {
public:
    /**
     * Initialise le serveur web :
     * - Configure toutes les routes
     * - Démarre le serveur
     */
    static void init();

private:
    // Instance du serveur asynchrone (port 80)
    static AsyncWebServer server;

    // Handlers pour chaque route
    static void handleRoot(AsyncWebServerRequest *request);
    static void handleApToggle(AsyncWebServerRequest *request);
    static void handleReset(AsyncWebServerRequest *request);

    // Handlers pour la gestion des logs
    static void handleLogs(AsyncWebServerRequest *request);
    static void handleLogsDownload(AsyncWebServerRequest *request);
    static void handleLogsClear(AsyncWebServerRequest *request);
};