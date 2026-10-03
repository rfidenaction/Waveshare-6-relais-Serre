// Actuators/LightManager.h
// Pilote logique des lumières (manager métier "lumière").
//
// Manager métier unique des lumières.
//   Scanne NEO (Config/Neo.h) au démarrage, ramasse les entrées dont le
//   handler déclaré est son propre enqueueByEntity et les gère : file de
//   commandes thread-safe, état allumé/éteint, journalisation via DataBus.
//   Pilote les relais directement via digitalWrite sur RELAYS[].gpio.
//   Ne porte aucune notion de durée : une commande pose l'état demandé
//   (0 = OFF, 1 = ON) et le relais reste dans cet état jusqu'à la suivante.
//   Aucune programmation horaire non plus : GardenerManager et
//   ConditionalWatering refusent les commandes d'état.
//
// Aucun canal ne lui est affecté aujourd'hui sur cette carte : les six relais
// sont des vannes. Le manager tourne alors à vide, sans aucun slot. Confier un
// canal à une lumière se fait sur sa seule ligne de RELAYS[] — entity, command
// et enqueue — sans toucher à ce fichier ni à aucun autre.
//
// Principe — META comme clé unique :
//   Chaque lumière est identifiée de bout en bout par son DataId META
//   (Lighting7, Lighting8). Aucun "index" n'est exposé dans l'API publique.
//
// Source de vérité du câblage fonctionnel :
//   RELAYS[] dans Config/IO-Config.h reste la DÉCLARATION du câblage, lue une
//   seule fois au démarrage par Neo::build(). Une entité n'y figure que sur
//   une ligne : un relais, une lumière. Ensuite, toutes les recherches à
//   l'exécution passent par NEO. Le tableau interne slots[] ne porte que
//   l'état allumé/éteint et le canal, recopié pour éviter une recherche NEO à
//   chaque commutation.
//
// Cycle de vie — même silence que ValveManager avant VALVE_START_DELAY_MS :
//   handle() reste complètement inactive jusque-là, puis construit slots[],
//   crée la queue FreeRTOS et publie l'état initial des lumières affectées.
//
// Journalisation sur changement d'état :
//   - Console::info
//   - DataBus::publish(BusItem) avec id=LightingN, value=0.0f|1.0f
//     → distribution immédiate (MQTT, log SPIFFS, lastDataForWeb)
#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "Config/TimingConfig.h"
#include "Config/MetaDataModel.h"
#include "Config/Neo.h"

class LightManager {
public:
    // Borne supérieure du tableau de slots. Adossée à NEO plutôt qu'au nombre
    // de canaux de cette carte-ci : une lumière est forcément une entrée NEO,
    // donc le dépassement est impossible et aucun nombre n'est à réviser si le
    // matériel gagne des canaux. Le nombre effectif est déterminé à
    // l'exécution par le scan de NEO.
    static constexpr uint8_t LIGHT_MAX = (uint8_t)Neo::MAX;

    // États logiques (évite LOW/HIGH sans signification métier)
    static constexpr uint8_t LIGHT_OFF = 0;
    static constexpr uint8_t LIGHT_ON  = 1;

    // Unique tâche périodique enregistrée dans TaskManager.
    //
    //  - Avant VALVE_START_DELAY_MS : return immédiat, aucune action.
    //  - Au premier passage après le délai : construit slots[] depuis NEO,
    //    crée la queue FreeRTOS et publie l'état initial "Éteinte" des
    //    lumières affectées.
    //  - Ensuite : consomme la queue de commandes.
    static void handle();

    // Point d'entrée thread-safe unique pour le routage générique RELAYS[]
    // (cf. IO-Config.h). Signature commune à tous les managers d'actionneurs :
    // un pointeur &LightManager::enqueueByEntity est stocké ligne par ligne
    // dans RELAYS[] et invoqué par DataBus::routeCommand après parseCommand.
    // Appelable depuis n'importe quel thread (MQTT, HTTP, …).
    //
    // requestedState : 0 = OFF, 1 = ON. Toute autre valeur est ignorée à
    // l'exécution. Retourne true si empilée, false si la queue n'existe pas
    // encore (avant VALVE_START_DELAY_MS) ou si elle est pleine. Non-bloquant.
    static bool enqueueByEntity(DataId entity, uint32_t requestedState);

    // true une fois VALVE_START_DELAY_MS écoulé ET queue créée.
    static bool isReady();

private:
    // ─── Structure de commande interne (transportée via la queue FreeRTOS) ─
    // Consommée uniquement par handle() dans le thread TaskManager ; remplie
    // par enqueueByEntity depuis les threads producteurs (MQTT, HTTP…).
    struct LightCommand {
        DataId   id;              // DataId META de la lumière cible
        uint32_t requestedState;  // 0 = OFF, 1 = ON
    };

    // ─── Slot interne : état runtime d'une lumière ───────────────────────
    // Ce slot ne décrit pas le câblage : NEO le porte déjà (entité, canal
    // relais, commande liée). Le GPIO physique est piloté via RELAYS[].gpio
    // dans applyLightState, seule chose qui reste lue directement dans
    // IO-Config.h parce qu'elle décrit la carte et non l'affectation
    // fonctionnelle.
    struct LightSlot {
        DataId  id;       // DataId META de la lumière (clé de recherche)
        uint8_t relayCh;  // canal relais (1-based, cf. NeoEntry::relayCh)
        uint8_t state;    // LIGHT_OFF ou LIGHT_ON
    };

    static LightSlot     slots[LIGHT_MAX];
    static uint8_t       slotCount;         // entrées valides dans slots[]
    static bool          lightSystemReady;

    // Queue FreeRTOS de commandes entrantes, créée paresseusement au premier
    // handle() après VALVE_START_DELAY_MS.
    // Producteur : thread esp_mqtt ou thread HTTP. Consommateur : TaskManager.
    static QueueHandle_t cmdQueue;

    // Recherche linéaire dans slots[]. Retourne true si trouvé, remplit outSlot.
    static bool findSlot(DataId id, LightSlot*& outSlot);

    // Validation métier d'une commande dépilée, puis application si elle
    // change quelque chose. Exécutée dans le thread TaskManager.
    static void applyRequested(DataId id, uint32_t requestedState);

    // Application physique d'un nouvel état + journalisation.
    static void applyLightState(LightSlot& slot, uint8_t newState);

    // Construit slots[] à partir de NEO : pour chaque entrée dont ce manager
    // porte le handler, recopie le canal relais et initialise l'état runtime
    // (LIGHT_OFF). Appelée une seule fois, au premier handle() après
    // VALVE_START_DELAY_MS. Privée car elle touche au type interne LightSlot.
    static void buildSlotsFromNeo();
};
