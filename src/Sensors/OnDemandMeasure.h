// Sensors/OnDemandMeasure.h
// Mesure à la demande — déclenchement ponctuel d'une acquisition, sur requête
// venue de l'interface utilisateur par MQTT.
//
// Rôle : recevoir une demande, la faire exécuter par le module propriétaire de
// la donnée, et rien de plus. Ce module ne connaît aucun capteur, aucune
// adresse Modbus, aucun registre. Il ne publie rien lui-même : la publication
// reste faite par le module capteur via DataBus, donc par le chemin normal
// (validation META, horodatage VirtualClock, journal CSV, MQTT, page web).
//
// Construction de la vue id → propriétaire :
//   Même principe que ValveManager, qui scanne RELAYS[] au démarrage et se
//   projette une vue runtime slots[]. Ici il n'existe pas de table centrale
//   d'affectation des capteurs : l'appartenance id ↔ adresse Modbus vit dans
//   le descripteur SENSORS[] privé de chaque module producteur. Ce sont donc
//   les modules qui déclarent leurs DataId (measurableCount/measurableAt), et
//   OnDemandMeasure agrège ces déclarations une fois au démarrage.
//
//   Conséquence : aucune correspondance id → propriétaire n'est écrite à la
//   main nulle part. Brancher deux sondes de sol supplémentaires se limite à
//   deux lignes dans SoilSensorRS485::SENSORS[] ; le routage et la liste
//   publiée dans le schéma MQTT suivent au prochain démarrage.
//
// Découplage des threads — la raison d'être du slot de demande :
//   Les trois modules capteurs partagent Serial1 et leurs lectures Modbus sont
//   bloquantes. La seule exclusion mutuelle du système est le fait que
//   TaskManager exécute ses callbacks séquentiellement. Une mesure lancée
//   depuis le thread esp_mqtt écrirait sur Serial1 en même temps que la boucle
//   TaskManager et corromprait le bus.
//   Donc onRequest() (thread esp_mqtt) ne fait que valider et poser l'id dans
//   un slot, et handle() (thread TaskManager) exécute la mesure. Aucun mutex
//   n'est nécessaire : l'exclusion est obtenue par construction.
//
// Échec de mesure : silence. Un capteur muet ne publie rien, le module
// producteur émet déjà son Console::warn, et l'interface n'affiche simplement
// aucune valeur nouvelle. Aucun protocole de retour vers l'émetteur.
#pragma once

#include <Arduino.h>
#include "Config/MetaDataModel.h"

class OnDemandMeasure {
public:
    // Topic de demande. Hors de serre/data/ que l'interface capte en « # »,
    // et aligné sur la convention FromUser/ToUser de Gardener et Conditional.
    // Le ToUser du même canal servira aux historiques.
    static constexpr const char* ONDEMAND_TOPIC_FROM_USER = "serre/ondemand/FromUser";

    // Interroge les modules producteurs et construit la vue id → propriétaire.
    // À appeler après les init() des trois modules capteurs.
    static void init();

    // Exécute la demande en attente. Tâche TaskManager.
    // Bloquant le temps d'une transaction Modbus (~40 ms, ~220 ms sur timeout),
    // exactement comme les handle() périodiques des capteurs.
    static void handle();

    // Point d'entrée des demandes MQTT. Appelé depuis le thread esp_mqtt :
    // valide le payload, pose l'id dans le slot, rend la main. Aucune I/O bus.
    // Payload JSON attendu : {"op":"measure","id":N}
    static void onRequest(const char* data, int len);

    // ─── Énumération pour le schéma MQTT ─────────────────────────────────
    // Consommée par MqttManager::buildSchemaJson pour publier measurableIds.
    // L'interface n'encode ainsi aucune règle sur ce qui est mesurable.
    static uint8_t measurableCount();
    static DataId  measurableAt(uint8_t index);

private:
    static constexpr const char* TAG = "OnDemand";

    // Borne supérieure de la vue. Le nombre effectif est déterminé au
    // démarrage par l'interrogation des modules producteurs (16 aujourd'hui,
    // 20 avec les sondes de sol 5 et 6). Marge volontaire pour absorber
    // l'ajout de producteurs sans y revenir.
    static constexpr uint8_t MEASURABLE_MAX = 32;

    // Signature commune à tous les modules producteurs.
    // Retourne true si la mesure a été publiée.
    using MeasureFn = bool (*)(DataId id);

    // Vue runtime : un DataId et le module qui sait le mesurer.
    struct MeasurableSlot {
        DataId    id;
        MeasureFn measure;
    };

    static MeasurableSlot slots[MEASURABLE_MAX];
    static uint8_t        slotCount;

    // Slot de demande, unique.
    // Écrit par onRequest (thread esp_mqtt), consommé par handle()
    // (thread TaskManager). Pas de queue FreeRTOS : l'id est posé avant le
    // drapeau, et un uint8_t est atomique sur ESP32, donc handle() ne peut
    // jamais lire un id incohérent. La seule course résiduelle est qu'une
    // nouvelle demande arrivée entre la lecture du drapeau et celle de l'id
    // soit exécutée à la place de la précédente — deux demandes légitimes de
    // l'utilisateur, aucune conséquence.
    // Une demande arrivant slot occupé est ignorée (le slot se libère en une
    // période de handle()).
    static volatile bool    requestPending;
    static volatile uint8_t requestedId;

    // Agrège les déclarations des modules producteurs.
    static void buildSlotsFromSensors();

    // Recopie la liste d'un module dans slots[]. Accède directement aux
    // membres statiques slots[] et slotCount.
    static void collect(uint8_t count, DataId (*at)(uint8_t), MeasureFn measure);

    // Recherche linéaire dans slots[]. Coût négligeable (au plus 32 éléments).
    static bool findSlot(DataId id, MeasurableSlot*& outSlot);
};
