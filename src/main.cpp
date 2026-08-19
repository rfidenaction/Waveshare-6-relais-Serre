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

#include "Sensors/SupplyVoltage.h"     // Tension alim via Analog Input 8CH (B) RS485
#include "Sensors/SoilSensorRS485.h"   // Sondes de sol RS485 Modbus RTU
#include "Sensors/AirSensorRS485.h"    // Capteurs air RS485 Modbus RTU (Ebyte KTH2-R)
#include "Sensors/InboxSensorRS485.h"  // Capteur air boîtier RS485 (Ebyte KTH2-R, adresse 15)
#include "Sensors/OnDemandMeasure.h"   // Mesure à la demande (serre/ondemand/FromUser)

#include "Actuators/ValveManager.h"

#include "Gardener/GardenerManager.h"
#include "Gardener/ConditionalWatering.h"

#include "Storage/DataLogger.h"
#include "Storage/HistoryQuery.h"   // Historique à la demande (serre/history/FromUser)

#include "Web/WebServer.h"
#include "Utils/Console.h"
#include "Utils/StatusReport.h"

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

    // Historique à la demande — relit les journaux de DataLogger, donc initialisé
    // juste après lui. N'écrit jamais sur la flash.
    HistoryQuery::init();

    // Reconstruction lastDataForWeb depuis /datalog.csv
    WebServer::rebuildLastDataFromFlash();

    RTCManager::init();

    VirtualClock::init();

    WebServer::init();

    EventManager::init();
    EventManager::prime();

    TaskManagerMonitor::init();

    SupplyVoltage::init();

    SoilSensorRS485::init();
    Console::info("[SoilRS485] SoilSensorRS485 initialisé");

    AirSensorRS485::init();
    Console::info("[AirRS485] AirSensorRS485 initialisé");

    InboxSensorRS485::init();
    Console::info("[InboxRS485] InboxSensorRS485 initialisé");

    // Mesure à la demande — doit venir APRÈS les quatre modules capteurs :
    // init() les interroge pour construire sa vue id → propriétaire.
    OnDemandMeasure::init();
    Console::info("[OnDemand] OnDemandMeasure initialisé");

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
    MqttManager::loadFamilyNames();
    Console::info("[MQTT] MqttManager initialisé (flux DataBus::mqttQueue → MQTT → Bridge)");

    GardenerManager::init();
    Console::info("[Gardener] GardenerManager initialisé");

    ConditionalWatering::init();
    Console::info("[Conditional] ConditionalWatering initialisé");

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

    // DataLogger — drain de la logQueue DataBus, un record par appel : la
    // période est courte pour lisser la sérialisation, pas pour journaliser vite.
    TaskManager::addTask(
        []() { DataLogger::handle(); },
        DATALOGGER_HANDLE_PERIOD_MS
    );

    // MQTT — drain de la mqttQueue DataBus
    TaskManager::addTask(
        []() { MqttManager::handle(); },
        200
    );

    // WiFi status → DataBus. Le décalage de phase sert ici de première
    // publication : sans lui, la période horaire laisserait l'état WiFi absent
    // du journal pendant l'heure suivant chaque démarrage.
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
        WIFI_STATUS_UPDATE_INTERVAL_MS,
        WIFI_STATUS_PHASE_OFFSET_MS
    );

    TaskManager::addTask(
        []() { SmsManager::handle(); },
        2000
    );

    // Les quatre tâches suivantes se partagent Serial1. Leurs décalages de phase
    // les empêchent de tomber dans la même passe de boucle, où leurs
    // transactions Modbus bloquantes s'additionneraient (voir TimingConfig.h,
    // section « TaskManager — décalage de phase »).
    TaskManager::addTask(
        []() { SupplyVoltage::handle(); },
        SUPPLY_VOLTAGE_HANDLE_PERIOD_MS,
        SUPPLY_VOLTAGE_PHASE_OFFSET_MS
    );

    // Capteurs RS485 à mesure de température : chaque famille interroge UN
    // capteur par appel, en rotation. La période de la tâche est donc la cadence
    // plancher divisée par l'effectif de la famille, de sorte que chaque capteur
    // soit lu à cette cadence et pas plus vite (auto-échauffement). Ajouter ou
    // retirer un capteur dans SENSORS[] ajuste la période sans rien changer ici.
    TaskManager::addTask(
        []() { SoilSensorRS485::handle(); },
        RS485_TEMP_READ_PERIOD_MS / SoilSensorRS485::sensorCount(),
        SOIL_RS485_PHASE_OFFSET_MS
    );

    TaskManager::addTask(
        []() { AirSensorRS485::handle(); },
        RS485_TEMP_READ_PERIOD_MS / AirSensorRS485::sensorCount(),
        AIR_RS485_PHASE_OFFSET_MS
    );

    // Un seul capteur boîtier, pas de rotation : pas de division.
    TaskManager::addTask(
        []() { InboxSensorRS485::handle(); },
        RS485_TEMP_READ_PERIOD_MS,
        INBOX_RS485_PHASE_OFFSET_MS
    );

    // Mesure à la demande — exécute dans CE thread la mesure demandée par
    // MQTT, donc jamais en concurrence avec les quatre tâches capteurs
    // ci-dessus sur Serial1.
    TaskManager::addTask(
        []() { OnDemandMeasure::handle(); },
        ONDEMAND_HANDLE_PERIOD_MS
    );

    // Historique à la demande — une opération élémentaire par appel
    TaskManager::addTask(
        []() { HistoryQuery::handle(); },
        HISTORY_HANDLE_PERIOD_MS
    );

    TaskManager::addTask(
        []() { ValveManager::handle(); },
        100
    );

    TaskManager::addTask(
        []() { GardenerManager::handle(); },
        GARDENER_HANDLE_PERIOD_MS
    );

    TaskManager::addTask(
        []() { ConditionalWatering::handle(); },
        CONDITIONAL_HANDLE_PERIOD_MS
    );

    TaskManager::addTask(
        []() { SafeReboot::handle(); },
        SAFE_REBOOT_PERIOD_MS
    );

    TaskManager::addTask(
        []() { TaskManagerMonitor::checkSchedulerRegularity(); },
        TASKMON_CHECK_PERIOD_MS
    );

    TaskManager::addTask(
        []() { StatusReport::handle(); },
        STATUS_TASK_PERIOD_MS
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