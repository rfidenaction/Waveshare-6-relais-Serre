// Web/Pages/PageActuators.h
// Page de pilotage des actionneurs (vannes).
//
// Page HTML locale servie par le serveur embarqué. Utilisée quand aucun réseau
// externe n'est disponible (AP local uniquement). Ne dépend pas de MQTT.
//
// Les commandes sont envoyées via POST /command en text/plain, payload CSV
// 7 champs identique au format MQTT serre/cmd. Le serveur enchaîne
// DataBus::parseCommand (validation) → DataBus::publish
// (horodatage + distribution + routage via NEO).
//
// La liste est construite depuis META, filtrée par Neo::find. Seules les
// vannes installées sur cette carte apparaissent.
#pragma once

#include <Arduino.h>

class PageActuators {
public:
    /**
     * Retourne le code HTML complet de la page de pilotage des actionneurs.
     * Lit l'état actuel de chaque vanne via WebServer::hasLastData.
     */
    static String getHtml();
};