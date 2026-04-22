// Actuators/ValveManager.h
// Pilote logique des électrovannes (manager métier "vanne").
//
// Architecture en deux couches :
//   RelayManager (Actuators/RelayManager) — driver matériel pur. Possède les
//       GPIO, expose activate/deactivate par canal 1-based. Aucune notion
//       métier.
//   ValveManager (ce fichier) — scanne RELAYS[] (IO-Config.h) au démarrage,
//       ramasse les canaux dont l'entity est une vanne (Valve1..Valve6) et
//       les gère : file de commandes thread-safe, timers d'auto-fermeture,
//       journalisation via DataLogger. Il pilote les relais uniquement via
//       RelayManager ; il ne touche jamais aux GPIO directement.
//
// Principe — META comme clé unique :
//   Chaque vanne est identifiée de bout en bout par son DataId META
//   (Valve1..Valve6). Aucun "index 0..5" n'est exposé dans l'API publique.
//   Le tableau interne slots[] associe DataId ↔ canal relais, c'est le SEUL
//   endroit du code qui fait cette correspondance côté vannes.
//
// Cycle de vie — silence total avant VALVE_START_DELAY_MS :
//   - handle()      : unique tâche périodique, enregistrée dès le boot dans
//                      TaskManager. Elle reste COMPLÈTEMENT inactive tant que
//                      millis() < VALVE_START_DELAY_MS (simple test + return,
//                      aucune allocation, aucune queue, aucun log).
//                      Au premier passage après le délai, elle construit
//                      slots[] depuis RELAYS[], crée la queue FreeRTOS et
//                      publie l'état initial des vannes affectées. Puis
//                      comportement normal.
//
//   Protection matérielle au boot (mise en LOW des GPIO) : portée par
//   RelayManager::initPinsSafe(), appelée avant tout autre init dans
//   main.cpp::setup().
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
    // Borne supérieure : au plus 6 vannes peuvent être affectées
    // simultanément dans RELAYS[] (la carte n'a que 6 canaux). Le nombre
    // effectif est déterminé à l'exécution par le scan de RELAYS[].
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
    // Unique tâche périodique (1000 ms) enregistrée dans TaskManager.
    //
    //  - Avant VALVE_START_DELAY_MS : return immédiat, aucune action.
    //  - Au premier passage après le délai : construit slots[] depuis
    //    RELAYS[], crée la queue FreeRTOS et publie l'état initial "Fermée"
    //    des vannes affectées.
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

    // ─── Pont commande ↔ observation ─────────────────────────────────────
    // Chaque vanne META (Valve1..Valve6) a une commande META associée
    // (CommandValve1..CommandValve6). Le lien est porté par ValveSlot
    // (rempli dans buildSlotsFromRelays). Ces deux fonctions exposent ce
    // lien aux émetteurs (MqttManager, WebServer) et aux consommateurs de
    // schéma JSON, sans passer par une table de correspondance séparée.
    //
    // Retour : true si trouvé, out rempli. false si non trouvé, out inchangé.
    // Pattern bool+out aligné sur DataLogger::hasLastDataForWeb.

    // Sens vanne → commande. Utilisé par les émetteurs avant pushCommand.
    static bool commandIdForValve(DataId valveId, DataId& out);

    // Sens commande → vanne. Utilisé par les générateurs de schéma JSON.
    static bool getCommandTargetFor(DataId cmdId, DataId& out);

private:
    // ─── Slot interne : associe DataId ↔ canal relais + état runtime ─────
    // Rempli au démarrage par scan de RELAYS[]. Seul endroit du code qui
    // fait la correspondance DataId vanne ↔ canal relais. Le GPIO physique
    // est un détail porté par RelayManager, invisible ici.
    struct ValveSlot {
        DataId   id;        // DataId META de la vanne (clé de recherche)
        DataId   commandId; // DataId META de la commande associée (CommandValveN)
        uint8_t  relayCh;   // canal RelayManager (1-based, cf. RELAYS[].ch)
        uint8_t  state;     // VALVE_CLOSED ou VALVE_OPENED
        uint32_t deadline;  // millis() cible de fermeture, 0 si fermée
    };

    static ValveSlot  slots[VALVE_COUNT];
    static uint8_t    slotCount;         // nombre d'entrées valides dans slots[]
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

    // Construit slots[] à partir de RELAYS[] (IO-Config.h).
    // Appelée une seule fois, au premier handle() après VALVE_START_DELAY_MS.
    // Privée car elle touche au type interne ValveSlot.
    static void buildSlotsFromRelays();
};