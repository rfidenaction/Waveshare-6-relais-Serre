// main.cpp
// Point d'entrée principal du système
// Rôle : orchestration globale, aucune logique métier

#include <Arduino.h>
#include <SPIFFS.h>
#include <time.h>
#include <stdlib.h>

#include "Config/Config.h"
#include "Config/TimingConfig.h"

#include "Connectivity/WiFiManager.h"
#include "Connectivity/ManagerUTC.h"
#include "Connectivity/BridgeManager.h"     // *** AJOUT BRIDGE ***
#include "Connectivity/MqttManager.h"
#include "Connectivity/SmsManager.h"        // *** AJOUT SMS ***

#include "Core/TaskManager.h"
#include "Core/TaskManagerMonitor.h"
#include "Core/EventManager.h"
#include "Core/RTCManager.h"
#include "Core/VirtualClock.h"
#include "Core/SafeReboot.h"

#include "Sensors/DataAcquisition.h"
#include "Sensors/FakeVoltage.h"       // TEST — À SUPPRIMER en production

#include "Actuators/RelayManager.h"
#include "Actuators/ValveManager.h"

#include "Storage/DataLogger.h"

#include "Web/WebServer.h"
#include "Utils/Console.h"

// -----------------------------------------------------------------------------
// Cycle de vie système : INIT → RUN
// -----------------------------------------------------------------------------

static unsigned long bootTimeMs = 0;

// ⚠️ Symbole global unique (utilisé par PagePrincipale)
unsigned long startTime = 0;

// Prototypes des boucles internes
static void loopInit();
static void loopRun();

// Pointeur vers la loop active
static void (*currentLoop)() = loopInit;

// -----------------------------------------------------------------------------
// SETUP — minimal, la console série n'est pas encore prête
// -----------------------------------------------------------------------------

void setup()
{
    delay(200);

    Console::begin(Console::Level::INFO);

    // *** PROTECTION MATÉRIELLE IMMÉDIATE ***
    // Force les 6 GPIO relais en OUTPUT LOW (= relais désactivé) dès le début
    // de setup(), avant toute autre init logicielle. Empêche tout état
    // flottant au boot qui pourrait coller un relais brièvement.
    // Portée par RelayManager (driver matériel pur) : aucune allocation,
    // aucune queue, aucune notion métier. Les managers métier (ValveManager
    // aujourd'hui) démarrent plus tard, paresseusement.
    RelayManager::initPinsSafe();
    // *** FIN PROTECTION MATÉRIELLE ***

    bootTimeMs = millis();
    startTime  = bootTimeMs;

    // Système de fichiers (pas de log, série pas prête)
    SPIFFS.begin(true);

    // WiFi (machine d'états, démarre en différé)
    WiFiManager::init();

    // Capteurs matériels
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
    // Attente de stabilisation système
    if (millis() - bootTimeMs < SYSTEM_INIT_DELAY_MS) {
        return;
    }

    // -------------------------------------------------------------------------
    // Transition INIT → RUN (une seule fois)
    // -------------------------------------------------------------------------

    Console::info("Entrée en régime permanent");

    // ─── Configuration timezone (une seule fois, pour tout le firmware) ───
    setenv("TZ", SYSTEM_TIMEZONE, 1);
    tzset();

    // -------------------------------------------------------------------------
    // Boot AP (séquence bloquante, une seule fois au boot)
    // -------------------------------------------------------------------------
    while (!WiFiManager::isAPEnabled()) {
        WiFiManager::handle();
        delay(100);
    }
    unsigned long stabStart = millis();
    while (millis() - stabStart < 1200) {
        WiFiManager::handle();
        delay(100);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // À partir d'ici la console série est prête, on peut loguer
    // ─────────────────────────────────────────────────────────────────────────

    Console::info("Boot " + String(DEVICE_NAME) + " v" + String(FW_VERSION));

    // --- SPIFFS ---
    if (SPIFFS.totalBytes() > 0) {
        Console::info("[SPIFFS] OK");
    } else {
        Console::error("[SPIFFS] ÉCHEC — pas de stockage flash");
    }

    // --- DataLogger ---
    DataLogger::init();
    Console::info("[DataLogger] OK");

    // --- RTC DS3231 ---
    RTCManager::init();

    // --- Horloge virtuelle machine ---
    VirtualClock::init();

    {
        time_t rtcTime;
        if (RTCManager::read(rtcTime)) {
            VirtualClock::sync(rtcTime);
            Console::info("[VClock] VirtualClock mise à jour sur RTC");
        } else {
            Console::warn("[VClock] RTC indisponible — démarrage à 12h30 arbitraire");
        }
    }

    // --- Serveur Web ---
    WebServer::init();

    // --- EventManager ---
    EventManager::init();
    EventManager::prime();

    // --- TaskManagerMonitor ---
    TaskManagerMonitor::init();

    // --- FakeVoltage (TEST) ---
    FakeVoltage::init();

    // Note : ValveManager n'est PAS initialisé ici. Les GPIO ont été forcés
    // à LOW dès setup() par RelayManager::initPinsSafe(). La construction
    // des slots depuis RELAYS[], la création de la queue FreeRTOS et la
    // publication de l'état initial sont différées et gérées paresseusement
    // par ValveManager::handle() au premier passage après VALVE_START_DELAY_MS.

    // --- SafeReboot ---
    SafeReboot::init();

    // *** AJOUT BRIDGE *** — Initialisation BridgeManager (UDP vers LilyGo)
    BridgeManager::init();
    Console::info("[Bridge] BridgeManager initialisé");
    // *** FIN AJOUT BRIDGE ***

    // *** MQTT *** — Initialisation MQTT + callbacks
    MqttManager::init();
    DataLogger::setOnPush(MqttManager::onDataPushed);
    MqttManager::setOnPublishSuccess(BridgeManager::onMqttPublish);
    Console::info("[MQTT] Callbacks DataLogger → MQTT → Bridge configurés");
    // *** FIN MQTT ***

    // *** SMS *** — Initialisation SmsManager (logique métier SMS)
    SmsManager::init();
    Console::info("[SMS] SmsManager initialisé");
    // *** FIN SMS ***

    // --- TaskManager ---
    TaskManager::init();

    // ─────────────────────────────────────────────────────────────────────────
    // Enregistrement des tâches périodiques
    // ─────────────────────────────────────────────────────────────────────────

    // WiFi
    TaskManager::addTask(
        []() { WiFiManager::handle(); },
        WIFI_HANDLE_PERIOD_MS
    );

    // NTP
    ManagerUTC::init();
    TaskManager::addTask(
        []() { ManagerUTC::handle(); },
        UTC_HANDLE_PERIOD_MS
    );

    // *** AJOUT BRIDGE *** — Tâche BridgeManager (UDP vers LilyGo)
    TaskManager::addTask(
        []() { BridgeManager::handle(); },
        BRIDGE_HANDLE_PERIOD_MS
    );
    // *** FIN AJOUT BRIDGE ***

    // EventManager
    TaskManager::addTask(
        []() { EventManager::handle(); },
        EVENT_MANAGER_PERIOD_MS
    );

    // DataLogger
    TaskManager::addTask(
        []() { DataLogger::handle(); },
        DATALOGGER_HANDLE_PERIOD_MS
    );

    // *** MQTT *** — Drain du tampon FIFO amont (Variante 1)
    // Démarre paresseusement le client esp_mqtt et publie UNE entrée du tampon
    // par tour. Non-bloquant. Voir MqttManager.h pour la doc complète.
    TaskManager::addTask(
        []() { MqttManager::handle(); },
        1000
    );
    // *** FIN MQTT ***

    // WiFi status → DataLogger
    TaskManager::addTask(
        []() {
            DataLogger::push(
                DataId::WifiStaConnected,
                WiFiManager::isSTAConnected() ? 1.0f : 0.0f
            );
            DataLogger::push(
                DataId::WifiApEnabled,
                WiFiManager::isAPEnabled() ? 1.0f : 0.0f
            );
            if (WiFiManager::isSTAConnected()) {
                DataLogger::push(DataId::WifiRssi, (float)WiFi.RSSI());
            } else {
                DataLogger::push(DataId::WifiRssi, -100.0f);
            }
        },
        WIFI_STATUS_UPDATE_INTERVAL_MS
    );

    // *** AJOUT SMS *** — Tâche SmsManager (logique métier SMS)
    TaskManager::addTask(
        []() { SmsManager::handle(); },
        2000
    );
    // *** FIN AJOUT SMS ***

    // FakeVoltage (TEST)
    TaskManager::addTask(
        []() { FakeVoltage::handle(); },
        30000
    );

    // ValveManager — unique tâche côté vannes. Dormante (if/return) tant que
    // millis() < VALVE_START_DELAY_MS, puis démarrage paresseux automatique.
    TaskManager::addTask(
        []() { ValveManager::handle(); },
        1000
    );

    // SafeReboot (reboot préventif mensuel)
    TaskManager::addTask(
        []() { SafeReboot::handle(); },
        SAFE_REBOOT_PERIOD_MS
    );

    // TaskManagerMonitor — supervision de la régularité du scheduler
    // Placée en dernière position : sentinelle, observe l'ensemble du système.
    TaskManager::addTask(
        []() { TaskManagerMonitor::checkSchedulerRegularity(); },
        TASKMON_CHECK_PERIOD_MS
    );

    // Bascule définitive vers la loop de production
    currentLoop = loopRun;
}

// -----------------------------------------------------------------------------
// Phase RUN (production)
// -----------------------------------------------------------------------------

static void loopRun()
{
    TaskManager::handle();
}