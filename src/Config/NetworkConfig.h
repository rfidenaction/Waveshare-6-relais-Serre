// src/Config/NetworkConfig.h
#pragma once
#include <IPAddress.h>

// -----------------------------------------------------------------------------
// WiFi — STA (connexion vers LilyGo AP "Pont_Wifi-GSM_de_la_serre")
// -----------------------------------------------------------------------------
static constexpr const char* WIFI_STA_SSID     = "Pont_Wifi-GSM_de_la_serre";
static constexpr const char* WIFI_STA_PASSWORD = "1234567890";
static const IPAddress WIFI_STA_IP      (192,168,4,10);
static const IPAddress WIFI_STA_GATEWAY (192,168,4,1);
static const IPAddress WIFI_STA_SUBNET  (255,255,255,0);
static const IPAddress WIFI_STA_DNS     (8,8,8,8);

// -----------------------------------------------------------------------------
// WiFi — AP
// -----------------------------------------------------------------------------
static constexpr const char* WIFI_AP_SSID     = "Serre_de_Marie-Pierre";
static constexpr const char* WIFI_AP_PASSWORD = "1234567890";
static const IPAddress WIFI_AP_IP      (192,168,5,1);
static const IPAddress WIFI_AP_GATEWAY (192,168,5,1);
static const IPAddress WIFI_AP_SUBNET  (255,255,255,0);

// =============================================================================
// SMS - Numéros de destination
// (envoi via WiFi → LilyGo — mécanisme à définir)
// Format international avec "+"
// =============================================================================
static constexpr const char* SMS_NUMBERS[] = {
    "+33672967933"
    // Ajouter d'autres numéros ici si besoin :
    // "+33698765432",
    // "+33611223344"
};
static constexpr size_t SMS_NUMBERS_COUNT = sizeof(SMS_NUMBERS) / sizeof(SMS_NUMBERS[0]);