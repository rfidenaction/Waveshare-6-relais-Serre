// Core/EventManager.cpp
// Portage Waveshare ESP32-S3-Relay-6CH
// Suppression : section Power (pas de PMU/batterie sur cette carte)

#include "Core/EventManager.h"

#include "Connectivity/WiFiManager.h"
#include <WiFi.h>

// -----------------------------------------------------------------------------
// États statiques
// -----------------------------------------------------------------------------

EventManager::WifiState  EventManager::currentWifi;
EventManager::WifiState  EventManager::previousWifi;

// -----------------------------------------------------------------------------
// Initialisation
// -----------------------------------------------------------------------------

void EventManager::init()
{
    currentWifi.valid   = false;
    previousWifi.valid  = false;
}

// -----------------------------------------------------------------------------
// Amorçage des états (INIT → RUN)
// -----------------------------------------------------------------------------

void EventManager::prime()
{
    readWifiState(currentWifi);
    previousWifi = currentWifi;
}

// -----------------------------------------------------------------------------
// Point d'entrée périodique
// -----------------------------------------------------------------------------

void EventManager::handle()
{
    // Sauvegarde de l'état précédent
    previousWifi  = currentWifi;

    // Lecture du nouvel état
    readWifiState(currentWifi);
}

// -----------------------------------------------------------------------------
// Lecture WiFiManager
// -----------------------------------------------------------------------------

void EventManager::readWifiState(WifiState& target)
{
    target.staEnabled   = true;  // STA toujours actif sur Waveshare
    target.staConnected = WiFiManager::isSTAConnected();

    if (target.staConnected) {
        target.rssi = WiFi.RSSI();
    } else {
        target.rssi = 0;
    }

    target.valid = true;
}

// -----------------------------------------------------------------------------
// Accesseurs – WiFi
// -----------------------------------------------------------------------------

bool EventManager::hasWifiState()                { return currentWifi.valid; }
bool EventManager::hasPreviousWifiState()        { return previousWifi.valid; }

bool EventManager::isStaEnabled()                { return currentWifi.staEnabled; }
bool EventManager::wasStaEnabled()               { return previousWifi.staEnabled; }

bool EventManager::isStaConnected()              { return currentWifi.staConnected; }
bool EventManager::wasStaConnected()             { return previousWifi.staConnected; }

int EventManager::getRssi()                      { return currentWifi.rssi; }
int EventManager::getPreviousRssi()              { return previousWifi.rssi; }