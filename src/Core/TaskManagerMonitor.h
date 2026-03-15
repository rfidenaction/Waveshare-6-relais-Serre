// Core/TaskManagerMonitor.h
#pragma once

#include <Arduino.h>

/*
 * TaskManagerMonitor
 *
 * Supervision de la régularité d’appel d’EventManager (et donc indirectement
 * du TaskManager).
 *
 * Objectif :
 *  - détecter une dérive significative du rythme attendu
 *  - basculer dans un état WARNING (latched)
 *  - horodater l’événement
 *  - permettre un acquittement manuel (interface web)
 *
 * États :
 *  - OK       : rythme conforme
 *  - WARNING  : rythme hors plage, persistant jusqu’à acquittement
 *
 * IMPORTANT :
 *  - aucun correctif automatique
 *  - aucune action bloquante
 *  - tous les timings viennent de TimingConfig.h
 */

class TaskManagerMonitor {
public:
    // Initialisation
    static void init();

    // À appeler à chaque passage d’EventManager
    static void notifyCall();

    // État
    static bool     isWarningActive();
    static uint32_t getWarningTimestampMs();

    // Action utilisateur (acquittement)
    static void acknowledgeWarning();

private:
    static uint32_t lastCallMs;
    static bool     warningActive;
    static uint32_t warningTimestampMs;

    static void evaluateDelta(uint32_t nowMs);
};
