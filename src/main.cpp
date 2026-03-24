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

#include "Core/TaskManager.h"
#include "Core/EventManager.h"
#include "Core/RTCManager.h"
#include "Core/VirtualClock.h"
#include "Core/SafeReboot.h"

#include "Sensors/DataAcquisition.h"
#include "Sensors/FakeVoltage.h"       // TEST — À SUPPRIMER en production

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

    if (RTCManager::is_RTC_available()) {
        time_t rtcTime;
        if (RTCManager::read(rtcTime)) {
            VirtualClock::sync(rtcTime);
            Console::info("[VClock] VirtualClock mise à jour sur RTC");
        } else {
            Console::warn("[VClock] RTC disponible mais lecture échouée");
        }
    } else {
        Console::warn("[VClock] RTC indisponible — démarrage à 12h30 arbitraire");
    }

    // --- Serveur Web ---
    WebServer::init();

    // --- EventManager ---
    EventManager::init();
    EventManager::prime();

    // --- FakeVoltage (TEST) ---
    FakeVoltage::init();

    // --- SafeReboot ---
    SafeReboot::init();

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

    // WiFi status → DataLogger
    TaskManager::addTask(
        []() {
            DataLogger::push(
                DataType::System,
                DataId::WifiStaConnected,
                WiFiManager::isSTAConnected() ? 1.0f : 0.0f
            );
            DataLogger::push(
                DataType::System,
                DataId::WifiApEnabled,
                WiFiManager::isAPEnabled() ? 1.0f : 0.0f
            );
            if (WiFiManager::isSTAConnected()) {
                DataLogger::push(DataType::System, DataId::WifiRssi, (float)WiFi.RSSI());
            } else {
                DataLogger::push(DataType::System, DataId::WifiRssi, -100.0f);
            }
        },
        WIFI_STATUS_UPDATE_INTERVAL_MS
    );

    // FakeVoltage (TEST)
    TaskManager::addTask(
        []() { FakeVoltage::handle(); },
        30000
    );

    // SafeReboot (reboot préventif mensuel)
    TaskManager::addTask(
        []() { SafeReboot::handle(); },
        SAFE_REBOOT_PERIOD_MS
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