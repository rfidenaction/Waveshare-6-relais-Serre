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

#include "Sensors/DataAcquisition.h"
#include "Sensors/FakeVoltage.h"       // TEST — À SUPPRIMER en production

#include "Storage/DataLogger.h"

#include "Web/WebServer.h"
#include "Utils/Console.h"

// -----------------------------------------------------------------------------
// Cycle de vie système : INIT → RUN
// -----------------------------------------------------------------------------

static unsigned long bootTimeMs = 0;

// -----------------------------------------------------------------------------
// Temps de fonctionnement (utilisé par l'interface web)
// -----------------------------------------------------------------------------
// ⚠️ Symbole global unique (utilisé par PagePrincipale)
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
    Console::info("Boot système");

    bootTimeMs = millis();
    startTime  = bootTimeMs;

    // --- Système de fichiers ---
    if (!SPIFFS.begin(true)) {
        Console::error("Erreur SPIFFS");
        // On continue quand même
    }

    DataLogger::init();

    // --- Connectivités ---
    WiFiManager::init();

    // --- Capteurs ---
    DataAcquisition::init();

    Console::info("Initialisation matérielle terminée");
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
    // Boot AP (one-shot, avant TaskManager)
    // On drive la machine d'états WiFi jusqu'à ce que l'AP soit opérationnel.
    // Séquence bloquante acceptable : traversée une seule fois au boot.
    // -------------------------------------------------------------------------
    while (!WiFiManager::isAPEnabled()) {
        WiFiManager::handle();
        delay(100);
    }
    // Laisser l'AP se stabiliser (état AP_STABILIZE = 1s dans WiFiManager)
    unsigned long stabStart = millis();
    while (millis() - stabStart < 1200) {
        WiFiManager::handle();
        delay(100);
    }

    // --- Serveur Web — démarré APRÈS que l'AP soit opérationnel ---
    WebServer::init();

    // Initialisation EventManager avec état stable
    EventManager::init();
    EventManager::prime();

    // Simulateur tension — TEST — À SUPPRIMER en production
    FakeVoltage::init();

    // Démarrage du TaskManager
    TaskManager::init();

    // -------------------------------------------------------------------------
    // TÂCHE WIFI (machine d'états non-bloquante)
    // -------------------------------------------------------------------------
    TaskManager::addTask(
        []() {
            WiFiManager::handle();
        },
        WIFI_HANDLE_PERIOD_MS
    );

    // -------------------------------------------------------------------------
    // INITIALISATION UTC / NTP
    // -------------------------------------------------------------------------
    ManagerUTC::init();

    // Tâche UTC / NTP (machine d'état autonome)
    TaskManager::addTask(
        []() {
            ManagerUTC::handle();
        },
        UTC_HANDLE_PERIOD_MS
    );

    // -------------------------------------------------------------------------
    // Tâche EventManager
    // -------------------------------------------------------------------------
    TaskManager::addTask(
        []() {
            EventManager::handle();
        },
        EVENT_MANAGER_PERIOD_MS
    );

    // -------------------------------------------------------------------------
    // TÂCHE DATALOGGER (flush SPIFFS + réparation UTC)
    // -------------------------------------------------------------------------
    TaskManager::addTask(
        []() {
            DataLogger::handle();
        },
        DATALOGGER_HANDLE_PERIOD_MS
    );

    // -------------------------------------------------------------------------
    // TÂCHE WI-FI → DataLogger
    // -------------------------------------------------------------------------
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
                DataLogger::push(
                    DataType::System,
                    DataId::WifiRssi,
                    (float)WiFi.RSSI()
                );
            } else {
                DataLogger::push(
                    DataType::System,
                    DataId::WifiRssi,
                    -100.0f
                );
            }
        },
        WIFI_STATUS_UPDATE_INTERVAL_MS
    );

    // -------------------------------------------------------------------------
    // TÂCHE FAKE VOLTAGE (test — À SUPPRIMER en production)
    // -------------------------------------------------------------------------
    TaskManager::addTask(
        []() {
            FakeVoltage::handle();
        },
        30000   // 30 secondes
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