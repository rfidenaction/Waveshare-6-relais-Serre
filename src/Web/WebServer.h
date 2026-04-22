// Web/WebServer.h
// Portage Waveshare ESP32-S3-Relay-6CH
// Changements :
//  - Suppression handleWifiToggle (STA toujours actif)
//  - Suppression handleGsmToggle (pas de modem cellulaire)
//  - Suppression handleGraphData : les graphiques sont servis via /logs/download
//    (bundle complet), le filtrage se fait côté client
//  - Ajout handleActuators (page web pilotage vannes)
//  - Ajout handleCommand (POST /command, body = CSV 7 champs, entrée unifiée
//    avec MQTT ; pipeline DataLogger::parseCommand → DataLogger::traceCommand
//    → CommandRouter::route)
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

    // Handlers pour la page Actionneurs (pilotage vannes)
    static void handleActuators(AsyncWebServerRequest *request);

    // POST /command — entrée commande unifiée (même format que MQTT serre/cmd).
    // Body = CSV 7 champs en text/plain. Le body arrive par chunks via
    // handleCommandBody ; handleCommandFinal est appelé une fois le body
    // complet reçu, lit la String accumulée dans request->_tempObject,
    // puis orchestre le pipeline en trois étapes disjointes :
    // DataLogger::parseCommand → DataLogger::traceCommand → CommandRouter::route.
    static void handleCommandBody(AsyncWebServerRequest *request,
                                  uint8_t *data, size_t len,
                                  size_t index, size_t total);
    static void handleCommandFinal(AsyncWebServerRequest *request);
};