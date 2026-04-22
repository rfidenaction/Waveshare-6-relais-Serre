// Actuators/RelayManager.h
// Driver matériel bas niveau des relais (Waveshare ESP32-S3-Relay-6CH).
//
// Responsabilité strictement matérielle :
//   - Initialise les GPIO des relais déclarés dans RELAYS[] (IO-Config.h)
//     en OUTPUT LOW dès le début du boot (protection matérielle immédiate).
//   - Expose activate / deactivate / isActive en termes de canal (1-based,
//     aligné sur la sérigraphie de la carte CH1..CH6).
//   - NE CONNAÎT AUCUNE NOTION MÉTIER : pas de DataId, pas de vanne, pas de
//     lumière, pas de durée, pas de timer. Du pilotage de fer, rien d'autre.
//
// Consommateurs : tout manager métier qui possède un ou plusieurs canaux
// (ValveManager aujourd'hui, futur LightManager / FanManager demain).
// Chaque manager repère ses canaux via RELAYS[].entity et appelle activate/
// deactivate pour agir physiquement.
//
// Aucune allocation dynamique, aucune queue FreeRTOS, aucune dépendance sur
// Console ou DataLogger. Ce module doit rester trivial et testable isolément.
#pragma once

#include <Arduino.h>

class RelayManager {
public:
    // À appeler TRÈS TÔT dans setup(), avant tout autre init logiciel.
    // Force tous les GPIO déclarés dans RELAYS[] en OUTPUT LOW. Remet aussi
    // le cache d'états internes à "désactivé".
    static void initPinsSafe();

    // Active le relais du canal `ch` (1-based). GPIO → HIGH.
    // Ignoré silencieusement si `ch` ne figure pas dans RELAYS[].
    static void activate(uint8_t ch);

    // Désactive le relais du canal `ch` (1-based). GPIO → LOW.
    // Ignoré silencieusement si `ch` ne figure pas dans RELAYS[].
    static void deactivate(uint8_t ch);

    // État logique courant (true = activé). Retourne false si `ch` inconnu.
    static bool isActive(uint8_t ch);
};
