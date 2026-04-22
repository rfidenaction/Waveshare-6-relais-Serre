// Config/IO-Config.h
#pragma once

#include <Arduino.h>
#include "Storage/DataLogger.h"       // DataId utilisé dans RELAYS[]
#include "Actuators/ValveManager.h"   // handler enqueueByEntity référencé dans RELAYS[]

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
// Table unique reliant chaque canal physique à TOUTE la chaîne fonctionnelle :
// entité pilotée (DataId META), commande META associée, et handler du manager
// propriétaire. C'est la source de vérité du câblage réel de l'installation
// ET du routage logiciel des commandes.
//
// Règles :
//   - Une ligne par canal physique, dans l'ordre des canaux (1..6).
//   - Une ligne ne porte qu'UNE entity (un contact sec = un état à la fois).
//   - Plusieurs lignes peuvent porter la MÊME entity (mapping N:1 — ex :
//     lumière alimentée en parallèle par deux relais). Dans ce cas, elles
//     doivent porter la MÊME command et le MÊME enqueue handler.
//   - Pour réaffecter un relais : changer entity + command + enqueue ici
//     puis recompiler. Si l'entity ou la command n'existe pas encore dans
//     DATA_ID_LIST (DataLogger.h), l'y ajouter d'abord. Si le manager cible
//     n'existe pas encore, le créer (avec sa fonction enqueueByEntity).
//
// Invariant — chaque relais a TOUJOURS une commande et un handler :
//   La carte n'a que 6 canaux et ils sont tous destinés à être pilotés par
//   une commande de durée (vanne aujourd'hui, ventilateur/éclairage demain).
//   Quand on réaffectera un canal vanne → ventilateur, on créera le couple
//   entity+command dans DATA_ID_LIST, le manager correspondant avec sa
//   fonction enqueueByEntity, et on remplacera les trois champs sur la
//   ligne concernée. Aucun cas « pas de commande » n'est prévu.
//
// Dispatch des commandes (zéro code ailleurs) :
//   CommandRouter::route (Core/CommandRouter) parcourt RELAYS[] à la
//   recherche d'une ligne où command == cmdId reçue. Trouvée → il appelle
//   ligne.enqueue(ligne.entity, durationMs). Les dispatchers (MqttManager,
//   WebServer) enchaînent DataLogger::parseCommand → DataLogger::traceCommand
//   → CommandRouter::route, sans jamais connaître les managers d'actionneurs.
//   Ajouter LightManager = créer son enqueueByEntity + changer quelques
//   lignes de RELAYS[], sans toucher au reste du code.
//
// Lecture par les managers métier :
//   RelayManager     : lit gpio/ch, sans regarder entity/command/enqueue.
//                      Couche matérielle pure.
//   ValveManager     : scanne la table, ramasse les lignes dont entity est
//                      une vanne (Valve1..Valve6) et construit ses slots
//                      (entity + ch). Ne regarde ni command ni enqueue :
//                      ces champs servent le dispatch, pas l'exécution.
//   Futurs managers  : même principe (LightManager ramasserait ses Light*).
// =============================================================================

// Signature commune à tous les handlers de manager (ValveManager::enqueueByEntity,
// future LightManager::enqueueByEntity, …). Reçoit l'entité à piloter et la
// durée en ms ; retourne true si la commande a pu être empilée vers le manager.
using RelayEnqueueFn = bool (*)(DataId entity, uint32_t durationMs);

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