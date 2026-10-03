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
// Qui sait mesurer quoi :
//   NEO (Config/Neo.h) le dit. Chaque module producteur y déclare, pour
//   chacune des données qu'il produit, le pointeur de mesure à appeler. Ce
//   module n'a donc aucune vue à construire ni à tenir : il cherche l'entrée
//   NEO de l'id demandé et appelle son champ measure. Un id absent de NEO,
//   ou dont l'entrée ne porte pas de pointeur de mesure, n'est pas mesurable.
//
//   Conséquence : brancher deux sondes de sol supplémentaires se limite à deux
//   lignes dans SoilSensorRS485::SENSORS[] ; le routage suit au prochain
//   démarrage, sans que ce fichier bouge.
//
// Découplage des threads — la raison d'être du slot de demande :
//   Les modules capteurs partagent Serial1 et leurs lectures Modbus sont
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

    // Arme le slot de demande. À appeler après Neo::build().
    static void init();

    // Exécute la demande en attente. Tâche TaskManager.
    // Bloquant le temps d'une transaction Modbus (~40 ms, ~220 ms sur timeout),
    // exactement comme les handle() périodiques des capteurs.
    static void handle();

    // Point d'entrée des demandes MQTT. Appelé depuis le thread esp_mqtt :
    // valide le payload, pose l'id dans le slot, rend la main. Aucune I/O bus.
    // Payload JSON attendu : {"op":"measure","id":N}
    static void onRequest(const char* data, int len);

private:
    static constexpr const char* TAG = "OnDemand";

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
};
