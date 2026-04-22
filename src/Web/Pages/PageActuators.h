// Web/Pages/PageActuators.h
// Page de pilotage des actionneurs (vannes).
//
// Page HTML locale servie par le serveur embarqué. Utilisée quand aucun réseau
// externe n'est disponible (AP local uniquement). Ne dépend pas de MQTT.
//
// Les commandes sont envoyées via POST /command en text/plain, payload CSV
// 7 champs identique au format MQTT serre/cmd. Le serveur enchaîne
// DataLogger::parseCommand (validation) → DataLogger::traceCommand
// (journalisation) → CommandRouter::route (dispatch vers le manager
// propriétaire via RELAYS[]). Même circuit que les commandes MQTT.
//
// La liste des vannes est construite dynamiquement depuis META : tous les
// DataIds dont type == Actuator et nature == etat sont affichés.
#pragma once

#include <Arduino.h>

class PageActuators {
public:
    /**
     * Retourne le code HTML complet de la page de pilotage des actionneurs.
     * Lit l'état actuel de chaque vanne via DataLogger::hasLastDataForWeb.
     */
    static String getHtml();
};