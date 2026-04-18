// Actuators/ValveManager.h
// Pilote des 6 électrovannes (relais actif HIGH sur ESP32-S3-Relay-6CH)
//
// Principe — META comme clé unique :
//   Chaque vanne est identifiée de bout en bout par son DataId META
//   (Valve1..Valve6). Aucun "index 0..5" n'est exposé dans l'API publique.
//   Le tableau interne slots[] associe DataId ↔ GPIO, c'est le SEUL endroit
//   du code qui fait cette correspondance.
//
// Cycle de vie — silence total avant VALVE_START_DELAY_MS :
//   - initPinsSafe() : force les 6 GPIO en OUTPUT + LOW (fermé) dès setup(),
//                      avant tout autre init logiciel. Protection matérielle
//                      immédiate contre un état flottant au boot.
//   - handle()      : unique tâche périodique, enregistrée dès le boot dans
//                      TaskManager. Elle reste COMPLÈTEMENT inactive tant que
//                      millis() < VALVE_START_DELAY_MS (simple test + return,
//                      aucune allocation, aucune queue, aucun log).
//                      Au premier passage après le délai, elle crée
//                      paresseusement la queue FreeRTOS et publie l'état
//                      initial des 6 vannes. Puis comportement normal.
//   - openFor()     : ouvre une vanne pour une durée donnée.
//                      Ignorée si la vanne est déjà ouverte (anti-rebond).
//                      Durée clampée à VALVE_MAX_DURATION_MS (sécurité métier).
//   - enqueueCommand() : point d'entrée thread-safe depuis d'autres threads
//                        (notamment mqttEventHandler dans le thread esp_mqtt).
//                        Si la queue n'existe pas encore (avant démarrage),
//                        retourne false et la commande est rejetée côté appelant.
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
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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

    // ─── Structure de commande (transportée via la queue FreeRTOS) ───────
    // Publique car construite par MqttManager (dispatcher serre/cmd/{id})
    // et par WebServer (POST /actuators/open).
    struct ValveCommand {
        DataId   id;          // DataId META correspondant à la vanne
        uint32_t durationMs;  // durée d'ouverture en millisecondes
    };

    // ─── Cycle de vie ────────────────────────────────────────────────────
    // À appeler TRÈS TÔT dans setup(), avant tout autre init logiciel.
    // Force les 6 GPIO en OUTPUT LOW pour garantir l'état fermé immédiat.
    // N'utilise ni Console, ni DataLogger, ni allocation, ni queue FreeRTOS.
    static void initPinsSafe();

    // Unique tâche périodique (1000 ms) enregistrée dans TaskManager.
    //
    //  - Avant VALVE_START_DELAY_MS : return immédiat, aucune action.
    //  - Au premier passage après le délai : crée la queue FreeRTOS et
    //    publie l'état initial "Fermée" des 6 vannes.
    //  - Ensuite : consomme la queue de commandes puis vérifie les
    //    deadlines de fermeture.
    static void handle();

    // ─── API publique ────────────────────────────────────────────────────
    // Ouvre la vanne identifiée par son DataId META, pour durationMs millisecondes.
    // Ignoré si :
    //   - système pas encore démarré (avant VALVE_START_DELAY_MS)
    //   - DataId inconnu (pas une vanne)
    //   - vanne déjà ouverte
    // Durée clampée à VALVE_MAX_DURATION_MS si dépassement.
    // Appelée depuis le thread TaskManager uniquement.
    static void openFor(DataId id, uint32_t durationMs);

    // Point d'entrée thread-safe pour producteurs externes (thread esp_mqtt,
    // handler HTTP, etc.).
    // Retourne true si acceptée, false si la queue n'existe pas encore
    // (avant VALVE_START_DELAY_MS) ou si elle est pleine.
    // Non-bloquant (timeout 0).
    static bool enqueueCommand(const ValveCommand& cmd);

    // true une fois VALVE_START_DELAY_MS écoulé ET queue créée.
    static bool isReady();

private:
    // ─── Slot interne : associe DataId ↔ GPIO + état runtime ─────────────
    // Un seul tableau de 6 entrées. Seul endroit du code qui fait la
    // correspondance DataId ↔ GPIO.
    struct ValveSlot {
        DataId   id;        // DataId META (clé de recherche)
        uint8_t  gpio;      // broche GPIO physique (depuis IO-Config.h)
        uint8_t  state;     // VALVE_CLOSED ou VALVE_OPENED
        uint32_t deadline;  // millis() cible de fermeture, 0 si fermée
    };

    static ValveSlot  slots[VALVE_COUNT];
    static bool       valveSystemReady;

    // Queue FreeRTOS de commandes entrantes. Taille = VALVE_COUNT.
    // nullptr tant que VALVE_START_DELAY_MS n'est pas écoulé.
    // Créée paresseusement au premier handle() après le délai.
    // Producteur : thread esp_mqtt ou thread HTTP (via enqueueCommand).
    // Consommateur : thread TaskManager (via handle).
    static QueueHandle_t cmdQueue;

    // Recherche linéaire dans slots[]. Retourne true si trouvé, remplit outSlot.
    // Coût négligeable (6 éléments).
    static bool findSlot(DataId id, ValveSlot*& outSlot);

    // Application physique d'un nouvel état + journalisation.
    static void applyValveState(ValveSlot& slot, uint8_t newState);
};