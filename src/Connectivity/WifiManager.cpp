// src/Connectivity/WiFiManager.cpp

#include <Arduino.h>
#include "Connectivity/WiFiManager.h"
#include <WiFi.h>
#include <esp_netif.h>
#include "Config/NetworkConfig.h"
#include "Utils/Console.h"

// =============================================================================
// Variables statiques
// =============================================================================

// Machine d'états
WiFiManager::State WiFiManager::state = WiFiManager::State::AP_CONFIG;

// États runtime
bool WiFiManager::staConnected  = false;
bool WiFiManager::apEnabled     = false;  // false tant que softAP() n'a pas réussi
uint8_t WiFiManager::staRetryCount = 0;

// Timing
unsigned long WiFiManager::apStabilizeStartMs = 0;
unsigned long WiFiManager::connectStartMs     = 0;
unsigned long WiFiManager::retryStartMs       = 0;
unsigned long WiFiManager::lastConnectLogMs   = 0;

// Flag de demande externe
bool WiFiManager::apDisableRequested = false;

// =============================================================================
// Initialisation — active la radio WiFi (lwIP) en mode AP+STA
// =============================================================================
void WiFiManager::init()
{
    // WiFi.mode() doit être appelé ici (dans setup()) car :
    // - Il initialise lwIP (pile TCP/IP), prérequis pour WebServer::init()
    // - ~50ms, acceptable dans setup()
    // - La suite (softAPConfig, softAP, STA) est gérée par la machine d'états
    WiFi.mode(WIFI_AP_STA);

    state = State::AP_CONFIG;

    Console::info("WiFi", "init — mode AP_STA");
}

// =============================================================================
// Transition d'état avec log
// =============================================================================
void WiFiManager::changeState(State newState)
{
    static const char* names[] = {
        "AP_CONFIG", "AP_START", "AP_STABILIZE",
        "STA_CONFIG", "STA_BEGIN",
        "STA_CONNECTING", "STA_CONNECTED", "STA_DISCONNECT",
        "STA_WAIT_RETRY"
    };

    int oldIdx = static_cast<int>(state);
    int newIdx = static_cast<int>(newState);

    Console::info("WiFi", String(names[oldIdx]) + " -> " + names[newIdx]);

    state = newState;
}

// =============================================================================
// Traduction wl_status_t en chaîne lisible (diagnostic)
// =============================================================================
const char* WiFiManager::wlStatusToString(wl_status_t status)
{
    switch (status) {
        case WL_IDLE_STATUS:     return "IDLE";
        case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL";
        case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
        case WL_CONNECTED:       return "CONNECTED";
        case WL_CONNECT_FAILED:  return "CONNECT_FAILED";
        case WL_CONNECTION_LOST: return "CONNECTION_LOST";
        case WL_DISCONNECTED:    return "DISCONNECTED";
        default:                 return "UNKNOWN";
    }
}

// =============================================================================
// Application des demandes externes (uniquement en états stables)
// =============================================================================
void WiFiManager::applyPendingRequests()
{
    if (apDisableRequested) {
        apDisableRequested = false;

        Console::info("WiFi", "application disableAP()");

        WiFi.softAPdisconnect(true);  // ~14ms
        WiFi.mode(WIFI_STA);          // ~0.04ms

        apEnabled = false;
        Console::info("WiFi", "AP éteint à chaud (réactivation uniquement par reboot)");
    }
}

// =============================================================================
// Machine d'états principale — appelée par TaskManager toutes les 250ms
// =============================================================================
void WiFiManager::handle()
{
    switch (state) {

    // =========================================================================
    // ZONE BOOT — traversée une seule fois, jamais revisitée
    // WiFi.mode() est appelé dans init(), on démarre directement à AP_CONFIG
    // =========================================================================

    case State::AP_CONFIG: {
        // Sur ESP32-S3, WiFi.mode(WIFI_AP_STA) peut activer l'AP implicitement
        // avec le DHCP par défaut (192.168.4.x). softAPdisconnect() force un
        // reset propre avant de reconfigurer.
        WiFi.softAPdisconnect(true);

        changeState(State::AP_START);
        break;
    }

    case State::AP_START: {
        // WiFi.softAP() : ~0.1ms (box présente) ou ~725ms (box absente)
        // ⚠️ C'est le SEUL appel potentiellement long (>100ms)
        // Il n'est exécuté qu'UNE SEULE FOIS dans toute la vie du programme
        unsigned long t0 = millis();
        bool ok = WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
        unsigned long dt = millis() - t0;

        // ─── Configuration IP via API ESP-IDF (fiable sur ESP32-S3) ───
        // WiFi.softAPConfig() est ignoré sur certaines versions du core.
        // L'API esp_netif garantit que le DHCP redémarre sur le bon subnet.
        if (ok) {
            esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            if (ap_netif) {
                esp_netif_dhcps_stop(ap_netif);

                esp_netif_ip_info_t ip_info;
                // ⚠️ Doit correspondre à WIFI_AP_IP/GATEWAY/SUBNET dans NetworkConfig.h
                IP4_ADDR(&ip_info.ip,      192, 168, 5, 1);
                IP4_ADDR(&ip_info.gw,      192, 168, 5, 1);
                IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
                esp_netif_set_ip_info(ap_netif, &ip_info);

                esp_netif_dhcps_start(ap_netif);
                Console::info("WiFi", "AP DHCP reconfiguré sur 192.168.5.x (esp_netif)");
            } else {
                Console::error("WiFi", "esp_netif handle AP introuvable");
            }
        }

        apEnabled = ok;  // Seule source de vérité pour l'état AP

        if (ok) {
            Console::info("WiFi", "AP démarré — IP: " + WiFi.softAPIP().toString()
                         + " (" + String(dt) + "ms)");
        } else {
            Console::error("WiFi", "softAP() ERREUR — AP non disponible (" + String(dt) + "ms)");
        }

        apStabilizeStartMs = millis();
        changeState(State::AP_STABILIZE);
        break;
    }

    case State::AP_STABILIZE: {
        // Attente non-bloquante de stabilisation du driver AP+STA
        // Sans ce délai, le premier WiFi.begin() échoue systématiquement
        // avec CONNECT_FAILED (driver pas prêt après softAP)
        if (millis() - apStabilizeStartMs >= AP_STABILIZE_MS) {
            Console::info("WiFi", "AP stabilisé (" + String(AP_STABILIZE_MS) + "ms)");
            changeState(State::STA_CONFIG);
        }
        break;
    }

    // =========================================================================
    // ZONE RÉGIME PERMANENT — boucle bornée, jamais de retour en zone boot
    // =========================================================================

    case State::STA_CONFIG: {
        // WiFi.config() : ~4ms
        WiFi.config(WIFI_STA_IP, WIFI_STA_GATEWAY, WIFI_STA_SUBNET, WIFI_STA_DNS);

        changeState(State::STA_BEGIN);
        break;
    }

    case State::STA_BEGIN: {
        // WiFi.begin() : ~2ms
        WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASSWORD);

        connectStartMs   = millis();
        lastConnectLogMs = connectStartMs;
        changeState(State::STA_CONNECTING);
        break;
    }

    case State::STA_CONNECTING: {
        // Polling WiFi.status() : <1ms
        wl_status_t status = WiFi.status();

        if (status == WL_CONNECTED) {
            staConnected = true;
            staRetryCount = 0;  // Backoff reset — retour au rythme agressif
            Console::info("WiFi", "STA connecté — IP: " + WiFi.localIP().toString()
                         + ", RSSI: " + String(WiFi.RSSI()) + " dBm");
            changeState(State::STA_CONNECTED);
            break;
        }

        unsigned long now = millis();

        // Log périodique toutes les 5s (diagnostic sans spam)
        if (now - lastConnectLogMs >= STA_CONNECT_LOG_MS) {
            lastConnectLogMs = now;
            unsigned long elapsed = now - connectStartMs;
            Console::info("WiFi", "STA connecting... "
                         + String(elapsed / 1000) + "s, status="
                         + wlStatusToString(status));
        }

        // Timeout ?
        if (now - connectStartMs > STA_CONNECT_TIMEOUT_MS) {
            Console::info("WiFi", "STA timeout ("
                         + String(STA_CONNECT_TIMEOUT_MS / 1000) + "s)"
                         + ", dernier status=" + wlStatusToString(status));
            staConnected = false;
            changeState(State::STA_DISCONNECT);
        }

        // Note : les demandes externes ne sont PAS traitées ici.
        // Elles seront appliquées après passage en état stable.
        break;
    }

    case State::STA_CONNECTED: {
        // État stable — on traite les demandes externes
        applyPendingRequests();

        // Vérification connexion
        if (WiFi.status() != WL_CONNECTED) {
            staConnected = false;
            Console::info("WiFi", "STA connexion perdue");
            changeState(State::STA_DISCONNECT);
        }
        break;
    }

    case State::STA_DISCONNECT: {
        // WiFi.disconnect() : ~0.5ms
        WiFi.disconnect();

        staConnected = false;
        staRetryCount++;  // Comptabilise l'échec pour le backoff
        retryStartMs = millis();

        changeState(State::STA_WAIT_RETRY);
        break;
    }

    case State::STA_WAIT_RETRY: {
        // État stable — on traite les demandes externes
        applyPendingRequests();

        // Backoff : 2 essais agressifs (30s), puis passage à 300s
        unsigned long delay = (staRetryCount <= STA_AGGRESSIVE_RETRIES)
                            ? STA_RETRY_DELAY_MS
                            : STA_SLOW_RETRY_DELAY_MS;

        if (millis() - retryStartMs > delay) {
            Console::info("WiFi", "STA retry #" + String(staRetryCount)
                         + " après " + String(delay / 1000) + "s");
            changeState(State::STA_CONFIG);
        }
        break;
    }

    }  // switch
}

// =============================================================================
// Demande de coupure AP (différée, appliquée en état stable)
// =============================================================================
void WiFiManager::disableAP()
{
    apDisableRequested = true;
    Console::info("WiFi", "disableAP() demandé (sera appliqué en état stable)");
}

// =============================================================================
// Accesseurs
// =============================================================================
bool WiFiManager::isSTAConnected()  { return staConnected; }
bool WiFiManager::isAPEnabled()     { return apEnabled; }

// =============================================================================
// Infos Web
// =============================================================================
String WiFiManager::getSTAStatus()
{
    return staConnected
        ? "Connecté à " + String(WIFI_STA_SSID) + " " + WIFI_STA_IP.toString()
          + " (" + String(WiFi.RSSI()) + " dBm)"
        : "Recherche " + String(WIFI_STA_SSID) + "...";
}

String WiFiManager::getAPStatus()
{
    return apEnabled
        ? String(WIFI_AP_SSID) + "  " + WIFI_AP_IP.toString()
        : "AP désactivé";
}