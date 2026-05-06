// main.cpp
// Point d'entrée principal du système
// Rôle : orchestration globale, aucune logique métier

#include <Arduino.h>
#include <LittleFS.h>
#include <time.h>
#include <stdlib.h>

#include "Config/Config.h"
#include "Config/TimingConfig.h"
#include "Config/IO-Config.h"

#include "Connectivity/WiFiManager.h"
#include "Connectivity/NTPManager.h"
#include "Connectivity/BridgeManager.h"
#include "Connectivity/MqttManager.h"
#include "Connectivity/SmsManager.h"

#include "Core/TaskManager.h"
#include "Core/TaskManagerMonitor.h"
#include "Core/EventManager.h"
#include "Core/RTCManager.h"
#include "Core/VirtualClock.h"
#include "Core/SafeReboot.h"
#include "Core/DataBus.h"

#include "Sensors/DataAcquisition.h"
#include "Sensors/FakeVoltage.h"       // TEST — À SUPPRIMER en production

#include "Actuators/ValveManager.h"

#include "Storage/DataLogger.h"

#include "Web/WebServer.h"
#include "Utils/Console.h"

// -----------------------------------------------------------------------------
// Cycle de vie système : INIT → RUN
// -----------------------------------------------------------------------------

static unsigned long bootTimeMs = 0;

// Symbole global unique (utilisé par PagePrincipale)
unsigned long startTime = 0;

// Prototypes des boucles internes
static void loopInit();
static void loopRun();

// Pointeur vers la loop active
static void (*currentLoop)() = loopInit;

// -----------------------------------------------------------------------------
// SETUP
// -----------------------------------------------------------------------------

void setup()
{
    delay(200);

    Console::begin(Console::Level::INFO);

    // Protection matérielle immédiate — force les 6 GPIO relais en OUTPUT LOW
    // dès le début de setup(), avant toute autre init logicielle.
    initAllRelayPinsSafe();

    bootTimeMs = millis();
    startTime  = bootTimeMs;

    LittleFS.begin(true);

    WiFiManager::init();

    DataAcquisition::init();
}

// -----------------------------------------------------------------------------
// LOOP Arduino (immuable)
// -----------------------------------------------------------------------------

void loop()
{
    currentLoop();
}

// -----------------------------------------------------------------------------
// Phase INIT
// -----------------------------------------------------------------------------

static void loopInit()
{
    if (millis() - bootTimeMs < SYSTEM_INIT_DELAY_MS) {
        return;
    }

    Console::info("Entrée en régime permanent");

    setenv("TZ", SYSTEM_TIMEZONE, 1);
    tzset();

    // Boot AP (séquence bloquante, une seule fois au boot)
    while (!WiFiManager::isAPEnabled()) {
        WiFiManager::handle();
        delay(100);
    }
    unsigned long stabStart = millis();
    while (millis() - stabStart < 1200) {
        WiFiManager::handle();
        delay(100);
    }

    Console::info("Boot " + String(DEVICE_NAME) + " v" + String(FW_VERSION));

    if (LittleFS.totalBytes() > 0) {
        Console::info("[LittleFS] OK");
    } else {
        Console::error("[LittleFS] ÉCHEC — pas de stockage flash");
    }

    // DataBus — créé AVANT DataLogger pour que logQueue existe
    DataBus::init();
    Console::info("[DataBus] OK");

    // DataLogger — logger SPIFFS pur
    DataLogger::init();
    Console::info("[DataLogger] OK");

    // Reconstruction lastDataForWeb depuis /datalog.csv
    WebServer::rebuildLastDataFromFlash();

    RTCManager::init();

    VirtualClock::init();

    WebServer::init();

    EventManager::init();
    EventManager::prime();

    TaskManagerMonitor::init();

    FakeVoltage::init();

    // Note : ValveManager n'est PAS initialisé ici. Les GPIO ont été forcés
    // à LOW dès setup() par initAllRelayPinsSafe(). La construction
    // des slots depuis RELAYS[], la création de la queue FreeRTOS et la
    // publication de l'état initial sont différées et gérées paresseusement
    // par ValveManager::handle() au premier passage après VALVE_START_DELAY_MS.

    SafeReboot::init();

    BridgeManager::init();
    Console::info("[Bridge] BridgeManager initialisé");

    MqttManager::init();
    MqttManager::setOnPublishSuccess(BridgeManager::onMqttPublish);
    Console::info("[MQTT] MqttManager initialisé (flux DataBus::mqttQueue → MQTT → Bridge)");

    SmsManager::init();
    Console::info("[SMS] SmsManager initialisé");

    // --- TaskManager ---
    TaskManager::init();

    // ─────────────────────────────────────────────────────────────────────────
    // Enregistrement des tâches périodiques
    // ─────────────────────────────────────────────────────────────────────────

    TaskManager::addTask(
        []() { WiFiManager::handle(); },
        WIFI_HANDLE_PERIOD_MS
    );

    NTPManager::init();
    TaskManager::addTask(
        []() { NTPManager::handle(); },
        NTP_HANDLE_PERIOD_MS
    );

    TaskManager::addTask(
        []() { VirtualClock::handle(); },
        VCLOCK_HANDLE_PERIOD_MS
    );

    TaskManager::addTask(
        []() { BridgeManager::handle(); },
        BRIDGE_HANDLE_PERIOD_MS
    );

    TaskManager::addTask(
        []() { EventManager::handle(); },
        EVENT_MANAGER_PERIOD_MS
    );

    // DataLogger — période 30 s (plus sur le chemin critique)
    TaskManager::addTask(
        []() { DataLogger::handle(); },
        DATALOGGER_HANDLE_PERIOD_MS
    );

    // MQTT — drain de la mqttQueue DataBus
    TaskManager::addTask(
        []() { MqttManager::handle(); },
        200
    );

    // WiFi status → DataBus
    TaskManager::addTask(
        []() {
            BusItem item = {};

            item.type       = getMeta(DataId::WifiStaConnected).type;
            item.id         = DataId::WifiStaConnected;
            item.valueKind  = 0;
            item.valueFloat = WiFiManager::isSTAConnected() ? 1.0f : 0.0f;
            DataBus::publish(item);

            item.type       = getMeta(DataId::WifiApEnabled).type;
            item.id         = DataId::WifiApEnabled;
            item.valueKind  = 0;
            item.valueFloat = WiFiManager::isAPEnabled() ? 1.0f : 0.0f;
            DataBus::publish(item);

            item.type       = getMeta(DataId::WifiRssi).type;
            item.id         = DataId::WifiRssi;
            item.valueKind  = 0;
            item.valueFloat = WiFiManager::isSTAConnected()
                            ? (float)WiFi.RSSI() : -100.0f;
            DataBus::publish(item);
        },
        WIFI_STATUS_UPDATE_INTERVAL_MS
    );

    TaskManager::addTask(
        []() { SmsManager::handle(); },
        2000
    );

    TaskManager::addTask(
        []() { FakeVoltage::handle(); },
        30000
    );

    TaskManager::addTask(
        []() { ValveManager::handle(); },
        100
    );

    TaskManager::addTask(
        []() { SafeReboot::handle(); },
        SAFE_REBOOT_PERIOD_MS
    );

    TaskManager::addTask(
        []() { TaskManagerMonitor::checkSchedulerRegularity(); },
        TASKMON_CHECK_PERIOD_MS
    );

    currentLoop = loopRun;
}

// -----------------------------------------------------------------------------
// Phase RUN (production)
// -----------------------------------------------------------------------------

static void loopRun()
{
    TaskManager::handle();
}
