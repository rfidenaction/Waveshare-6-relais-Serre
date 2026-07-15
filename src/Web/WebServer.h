// Web/WebServer.h
// Portage Waveshare ESP32-S3-Relay-6CH
//
// lastDataForWeb[] hébergé ici (protégé par portMUX).
// updateLastData() appelé par DataBus::distribute().
// hasLastData() appelé par les pages web.
// handleCommandFinal() utilise DataBus::parseCommand/publish.
#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <array>
#include "Config/MetaDataModel.h"

struct BusItem;  // forward declaration (défini dans Core/DataBus.h)

class WebServer {
public:
    static void init();

    // Mise à jour thread-safe de lastDataForWeb depuis DataBus::distribute().
    // Protégée par portMUX (spinlock ESP32, ~µs).
    static void updateLastData(const BusItem& item);

    // Lecture thread-safe de lastDataForWeb. Retourne true si une valeur
    // existe pour cet id, remplit out. Pour les pages web.
    static bool hasLastData(DataId id, LastDataForWeb& out);

    // Reconstruction de lastDataForWeb depuis /datalog.csv au boot.
    // Appelée depuis main.cpp après LittleFS.begin() et avant WebServer::init().
    static void rebuildLastDataFromFlash();

private:
    static AsyncWebServer server;

    // ───────────── lastDataForWeb — vue RAM pour l'UI web ─────────────
    static std::array<LastDataForWeb, META_COUNT> lastDataForWeb;
    static std::array<bool,           META_COUNT> lastDataForWebHas;
    static portMUX_TYPE lastDataMux;

    // Handlers pour chaque route
    static void handleRoot(AsyncWebServerRequest *request);
    static void handleApToggle(AsyncWebServerRequest *request);
    static void handleReset(AsyncWebServerRequest *request);

    static void handleLogs(AsyncWebServerRequest *request);
    static void handleLogsDownload(AsyncWebServerRequest *request);
    static void handleLogsClear(AsyncWebServerRequest *request);

    static void handleActuators(AsyncWebServerRequest *request);

    static void handleRS485(AsyncWebServerRequest *request);
    static void handleRS485SetAddr(AsyncWebServerRequest *request);
    static void handleRS485Exit(AsyncWebServerRequest *request);

    static void handleCommandBody(AsyncWebServerRequest *request,
                                  uint8_t *data, size_t len,
                                  size_t index, size_t total);
    static void handleCommandFinal(AsyncWebServerRequest *request);
};
