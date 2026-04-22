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
// MQTT — Broker HiveMQ Cloud
// =============================================================================
static constexpr const char* MQTT_BROKER_URI   = "mqtts://3db6155980d4483e8b8c3036fd0afd6f.s1.eu.hivemq.cloud:8883";
static constexpr const char* MQTT_USERNAME     = "Graindesable";
static constexpr const char* MQTT_PASSWORD     = "Chaperonrouge64";
static constexpr const char* MQTT_CLIENT_ID    = "serre-waveshare";
static constexpr const char* MQTT_LWT_TOPIC    = "serre/status/waveshare";
static constexpr const char* MQTT_SCHEMA_TOPIC = "serre/schema";
static constexpr int         MQTT_KEEPALIVE_S  = 90;

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
// MQTT — Certificat CA racine (TLS obligatoire sur HiveMQ Cloud)
//
// ISRG Root X1 (Let's Encrypt) — expire 2035-06-04
// Source : https://letsencrypt.org/certs/isrgrootx1.pem
// =============================================================================
static const char* MQTT_CA_CERT =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n";