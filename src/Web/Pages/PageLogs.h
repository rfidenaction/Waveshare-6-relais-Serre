// Web/Pages/PageLogs.h
// Portage Waveshare ESP32-S3-Relay-6CH
// Changements :
//  - Suppression paramètre gsmActive (pas de modem cellulaire sur cette carte)
#pragma once

#include <Arduino.h>
#include "Storage/DataLogger.h"  // Pour LogFileStats

class PageLogs {
public:
    /**
     * Retourne le code HTML complet de la page de gestion des logs
     * @param stats Statistiques du fichier de logs
     */
    static String getHtml(const LogFileStats& stats);
};