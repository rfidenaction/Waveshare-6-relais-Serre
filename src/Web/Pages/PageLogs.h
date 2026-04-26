// Web/Pages/PageLogs.h
// Portage Waveshare ESP32-S3-Relay-6CH
// Changements :
//  - Suppression paramètre gsmActive (pas de modem cellulaire sur cette carte)
//  - LogFileStats → FlashUsageStats (stats unifiées flash : programme + SPIFFS)
#pragma once

#include <Arduino.h>
#include "Storage/DataLogger.h"  // Pour FlashUsageStats

class PageLogs {
public:
    /**
     * Retourne le code HTML complet de la page de gestion des logs
     * @param stats Statistiques d'utilisation de la flash
     */
    static String getHtml(const FlashUsageStats& stats);
};