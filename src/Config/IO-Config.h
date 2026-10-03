// Config/IO-Config.h
#pragma once

#include <Arduino.h>
#include "Config/MetaDataModel.h"     // DataId utilisé dans RELAYS[]
#include "Config/Neo.h"               // NeoEntry produit par relayNeoAt
#include "Actuators/ValveManager.h"   // handler enqueueByEntity référencé dans RELAYS[]
#include "Actuators/LightManager.h"   // handler enqueueByEntity des lumières

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

// Signature commune à tous les handlers de manager.
// Second argument : durée en ms (vanne) ou état 0/1 (lumière).
using RelayEnqueueFn = bool (*)(DataId entity, uint32_t commandParam);

struct RelayAssignment {
    uint8_t        ch;       // 1-based, aligné sur la sérigraphie CH1..CH6
    uint8_t        gpio;     // GPIO physique (cf. #define ci-dessus)
    DataId         entity;   // entité fonctionnelle pilotée par ce relais
    DataId         command;  // commande META associée (toujours présente)
    RelayEnqueueFn enqueue;  // handler du manager propriétaire (toujours présent)
};

inline constexpr RelayAssignment RELAYS[] = {
    { 1, RELAY_CH1_PIN, DataId::Valve1, DataId::CommandValve1, &ValveManager::enqueueByEntity },
    { 2, RELAY_CH2_PIN, DataId::Valve2, DataId::CommandValve2, &ValveManager::enqueueByEntity },
    { 3, RELAY_CH3_PIN, DataId::Valve3, DataId::CommandValve3, &ValveManager::enqueueByEntity },
    { 4, RELAY_CH4_PIN, DataId::Valve4, DataId::CommandValve4, &ValveManager::enqueueByEntity },
    { 5, RELAY_CH5_PIN, DataId::Valve5, DataId::CommandValve5, &ValveManager::enqueueByEntity },
    { 6, RELAY_CH6_PIN, DataId::Valve6, DataId::CommandValve6, &ValveManager::enqueueByEntity },
};

inline constexpr size_t RELAYS_COUNT = sizeof(RELAYS) / sizeof(RELAYS[0]);

// =============================================================================
// Déclaration NEO — projection de RELAYS[] en liste plate
//
// Deux entrées par canal : l'entité pilotée et sa commande. Chacune porte le
// canal relais et l'id de l'autre, de sorte que la correspondance
// vanne ↔ commande se lise dans les deux sens sans table supplémentaire.
//
// Réaffecter un canal se fait toujours sur la seule ligne de RELAYS[] :
// l'inventaire NEO suit au prochain démarrage, et une entité absente d'ici
// (Lighting7 sur cette carte) n'est tout simplement pas installée.
//
// Grandeur::Aucune : un relais ne mesure rien. Ce qu'il est et comment il
// s'affiche se lit dans META (type, nature, libellés d'état).
// =============================================================================

inline uint8_t relayNeoCount()
{
    return (uint8_t)(RELAYS_COUNT * 2);
}

inline NeoEntry relayNeoAt(uint8_t index)
{
    if (index >= RELAYS_COUNT * 2) index = 0;   // garde : index hors bornes

    const RelayAssignment& relay    = RELAYS[index / 2];
    const bool             isEntity = (index % 2 == 0);

    NeoEntry entry = {};
    entry.id       = isEntity ? relay.entity  : relay.command;
    entry.grandeur = Grandeur::Aucune;
    entry.concerne = (relay.enqueue == &LightManager::enqueueByEntity)
                     ? Concerne::Serre : Concerne::Sol;
    entry.relayCh  = relay.ch;
    entry.idLie    = isEntity ? relay.command : relay.entity;
    entry.enqueue  = relay.enqueue;

    return entry;
}

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

// =============================================================================
// Protection matérielle au boot — force tous les GPIO relais en OUTPUT LOW.
// À appeler très tôt dans setup().
// =============================================================================

inline void initAllRelayPinsSafe()
{
    for (size_t i = 0; i < RELAYS_COUNT; i++) {
        pinMode(RELAYS[i].gpio, OUTPUT);
        digitalWrite(RELAYS[i].gpio, LOW);
    }
}