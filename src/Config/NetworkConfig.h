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
// ⚠️ ATTENTION : l'IP, la gateway et le subnet AP sont DUPLIQUÉS en dur
// dans WiFiManager.cpp (fonction handle(), état AP_START, appels IP4_ADDR).
// L'API esp_netif n'accepte pas les IPAddress Arduino, d'où la duplication.
// Toute modification ici DOIT être reportée manuellement dans WiFiManager.cpp.
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

// =============================================================================
// Bridge UDP — Communication Waveshare ↔ LilyGo
// =============================================================================
static constexpr uint16_t BRIDGE_UDP_PORT_LOCAL  = 5001;   // Port écoute Waveshare
static constexpr uint16_t BRIDGE_UDP_PORT_REMOTE = 5000;   // Port écoute LilyGo

// =============================================================================
// MQTT — Broker EMQX Serverless
// =============================================================================
static constexpr const char* MQTT_BROKER_URI   = "mqtts://of90099e.ala.eu-central-1.emqxsl.com:8883";
static constexpr const char* MQTT_USERNAME     = "serre12**";
static constexpr const char* MQTT_PASSWORD     = "mdpserre12**";
static constexpr const char* MQTT_CLIENT_ID    = "serre-waveshare";
static constexpr const char* MQTT_SCHEMA_TOPIC = "serre/schema";

// Inventaire du matériel réellement installé (table NEO), sur son propre topic
// retenu. Séparé du schéma parce que les deux ne changent pas au même rythme :
// renommer un libellé republie META seul, changer de matériel republie NEO
// seul. Les deux tables se joignent côté interface par DataId.
static constexpr const char* MQTT_NEO_TOPIC    = "serre/neo";
static constexpr int         MQTT_KEEPALIVE_S  = 90;

// Presence de la carte : l'interface publie "ping", la carte repond "pong"
// immediatement.
static constexpr const char* MQTT_PING_TOPIC_FROM_USER = "serre/ping/FromUser";
static constexpr const char* MQTT_PING_TOPIC_TO_USER   = "serre/ping/ToUser";

// Preferences d'affichage de l'interface (3e capteur des cartes Famille, noms
// libres des boitiers Capteurs). Elles vivaient dans le localStorage de chaque
// telephone ; la carte les heberge desormais pour que tous voient la meme
// chose. Elle ne s'en sert pour aucune decision : elle stocke et rediffuse.
static constexpr const char* MQTT_UIPREFS_TOPIC_FROM_USER = "serre/uiprefs/FromUser";
static constexpr const char* MQTT_UIPREFS_TOPIC_TO_USER   = "serre/uiprefs/ToUser";

// =============================================================================
// Signal MqttKo — Waveshare → LilyGo (UDP)
//
// Quand la connexion MQTT reste KO pendant un temps prolonge, la Waveshare
// envoie le paquet UDP "MqttKo" a la LilyGo, qui declenche immediatement
// une renegociation PPP complete (DATA→COMMAND→DATA + enable_napt()).
// But : resynchroniser le NAPT / DNS cote LilyGo quand le DNS Internet
// devient inaccessible depuis la Waveshare (getaddrinfo returns 202).
//
// Timing (stateless cote LilyGo, pas de cooldown) :
//   - Premier envoi : apres MQTT_KO_FIRST_DELAY_MS de deconnexion continue
//   - Repetitions  : toutes les MQTT_KO_REPEAT_DELAY_MS tant que MQTT reste KO
//   - Reset complet des temporisations a chaque MQTT_EVENT_CONNECTED
// =============================================================================
static constexpr uint32_t MQTT_KO_FIRST_DELAY_MS  =  5UL * 60UL * 1000UL;  //  5 min
static constexpr uint32_t MQTT_KO_REPEAT_DELAY_MS = 15UL * 60UL * 1000UL;  // 15 min

// =============================================================================
// MQTT — Certificat CA racine (TLS obligatoire sur EMQX)
//
// DigiCert Global Root G2 — expire 2038-01-15
// Source : Archive/EMQX/emqxsl-ca.crt
// =============================================================================
static const char* MQTT_CA_CERT =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
    "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
    "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
    "MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
    "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
    "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
    "2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
    "1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
    "q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
    "tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
    "vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
    "BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
    "5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
    "1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
    "NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
    "Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
    "8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
    "pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
    "MrY=\n"
    "-----END CERTIFICATE-----\n";