// Actuators/ValveManager.h
// Pilote des 6 électrovannes (relais actif HIGH sur ESP32-S3-Relay-6CH)
//
// Principe :
//   - initPinsSafe() : force les 6 GPIO en OUTPUT + LOW (fermé) dès setup(),
//                      avant tout autre init logiciel. Protection matérielle
//                      immédiate contre un état flottant au boot.
//   - init()        : reset des variables internes + enregistrement du deadline
//                      de démarrage. Les relais sont déjà fermés par initPinsSafe().
//   - handle()      : bascule en ready après VALVE_START_DELAY_MS, puis scrute
//                      les timers de fermeture.
//   - openFor()     : ouvre une vanne pour une durée donnée.
//                      Ignorée tant que le système n'est pas ready.
//                      Ignorée si la vanne est déjà ouverte (anti-rebond).
//                      Durée clampée à VALVE_MAX_DURATION_MS (sécurité métier).
//
// Index vannes : 0..5  (Valve1 = index 0, ..., Valve6 = index 5)
//
// Journalisation sur changement d'état :
//   - Console::info
//   - DataLogger::push(DataId::ValveN, 0.0f|1.0f)
//     → publication MQTT automatique via le callback existant
//
// Référentiel temporel :
//   VALVE_START_DELAY_MS vit dans TimingConfig.h (timing système de stabilisation).
//   VALVE_MAX_DURATION_MS vit ici (plafond métier de sécurité).
#pragma once

#include <Arduino.h>
#include "Config/TimingConfig.h"
#include "Storage/DataLogger.h"

class ValveManager {
public:
    // ─── Constantes métier ───────────────────────────────────────────────
    static constexpr uint8_t  VALVE_COUNT           = 6;
    static constexpr uint32_t VALVE_MAX_DURATION_MS = 15UL * 60UL * 1000UL; // 15 min

    // États logiques (évite LOW/HIGH sans signification métier)
    static constexpr uint8_t VALVE_CLOSED = 0;
    static constexpr uint8_t VALVE_OPENED = 1;

    // ─── Cycle de vie ────────────────────────────────────────────────────
    // À appeler TRÈS TÔT dans setup(), avant tout autre init logiciel.
    // Force les 6 GPIO en OUTPUT LOW pour garantir l'état fermé immédiat.
    // N'utilise ni Console, ni DataLogger, ni allocation.
    static void initPinsSafe();

    // Init logique standard (appelée dans loopInit, après Console prête).
    static void init();

    // Scrutation périodique (appelée toutes les 1000 ms par TaskManager).
    static void handle();

    // ─── API publique ────────────────────────────────────────────────────
    // Ouvre la vanne `valveIndex` (0..5) pour `valveDurationMs` millisecondes.
    // Ignoré si :
    //   - système pas encore ready (phase de stabilisation post-boot)
    //   - valveIndex hors plage
    //   - vanne déjà ouverte
    // Durée clampée à VALVE_MAX_DURATION_MS si dépassement.
    static void openFor(uint8_t valveIndex, uint32_t valveDurationMs);

    // true une fois VALVE_START_DELAY_MS écoulé.
    static bool isReady();

private:
    static const uint8_t valvePins[VALVE_COUNT];
    static uint8_t       valveStates   [VALVE_COUNT];
    static uint32_t      valveDeadlines[VALVE_COUNT];
    static bool          valveSystemReady;

    static DataId valveDataId(uint8_t valveIndex);
    static void   applyValveState(uint8_t valveIndex, uint8_t newState);
};