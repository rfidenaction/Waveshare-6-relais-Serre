// Config/IO-Config.h
#pragma once

#include <Arduino.h>
#include "Storage/DataLogger.h"   // pour DataId utilisé dans RELAYS[]

/*
 * IO-Config
 * 
 * Configuration des GPIO et pins physiques.
 * Carte : Waveshare ESP32-S3-Relay-6CH
 * MCU   : ESP32-S3 (dual-core Xtensa LX7 @ 240MHz)
 * 
 * Source : documentation Waveshare officielle (pinout PDF)
 */

// =============================================================================
// Relais — couche physique (6 canaux, actifs HIGH)
//
// Les #define ci-dessous ne décrivent QUE la carte : quel GPIO correspond à
// quel canal relais sérigraphié CH1..CH6. Aucune notion d'affectation
// fonctionnelle ici.
// =============================================================================

#define RELAY_CH1_PIN      1     // CH1
#define RELAY_CH2_PIN      2     // CH2
#define RELAY_CH3_PIN      41    // CH3
#define RELAY_CH4_PIN      42    // CH4
#define RELAY_CH5_PIN      45    // CH5
#define RELAY_CH6_PIN      46    // CH6

// =============================================================================
// Affectation des relais — couche fonctionnelle
//
// Table unique reliant chaque canal physique à l'entité fonctionnelle qu'il
// pilote (identifiée par son DataId META). Source de vérité sur le câblage
// réel de l'installation.
//
// Règles :
//   - Une ligne par canal physique, dans l'ordre des canaux (1..6).
//   - Une ligne ne porte qu'UNE entity (un contact sec = un état à la fois).
//   - Plusieurs lignes peuvent porter la MÊME entity (mapping N:1 — ex :
//     lumière alimentée en parallèle par deux relais).
//   - Pour réaffecter un relais : changer son entity ici puis recompiler.
//     Si la nouvelle entity n'existe pas encore dans DATA_ID_LIST
//     (DataLogger.h), l'y ajouter d'abord.
//
// Lecture par les managers métier :
//   RelayManager     : lit gpio/ch, sans regarder entity. Couche matérielle pure.
//   ValveManager     : scanne la table, ramasse les lignes dont entity est une
//                      vanne (Valve1..Valve6) et construit ses slots.
//   Futurs managers  : même principe (LightManager ramasserait ses Light*).
// =============================================================================

struct RelayAssignment {
    uint8_t ch;       // 1-based, aligné sur la sérigraphie CH1..CH6
    uint8_t gpio;     // GPIO physique (cf. #define ci-dessus)
    DataId  entity;   // entité fonctionnelle pilotée par ce relais
};

inline constexpr RelayAssignment RELAYS[] = {
    { 1, RELAY_CH1_PIN, DataId::Valve1 },
    { 2, RELAY_CH2_PIN, DataId::Valve2 },
    { 3, RELAY_CH3_PIN, DataId::Valve3 },
    { 4, RELAY_CH4_PIN, DataId::Valve4 },
    { 5, RELAY_CH5_PIN, DataId::Valve5 },
    { 6, RELAY_CH6_PIN, DataId::Valve6 },
};

inline constexpr size_t RELAYS_COUNT = sizeof(RELAYS) / sizeof(RELAYS[0]);

// =============================================================================
// RS485 (UART isolé)
// =============================================================================

#define RS485_TX_PIN       17
#define RS485_RX_PIN       18

// =============================================================================
// RTC DS3231 (module Pico-RTC-DS3231 via header Pico HAT, bus I2C)
// =============================================================================

#define RTC_SDA_PIN        4
#define RTC_SCL_PIN        5

// =============================================================================
// Buzzer passif (fréquence contrôlable par PWM)
// =============================================================================

#define BUZZER_PIN         21

// =============================================================================
// LED RGB (WS2812)
// =============================================================================

#define RGB_LED_PIN        38

// =============================================================================
// Bouton BOOT
// =============================================================================

#define BOOT_BUTTON_PIN    0