#pragma once

#include <Arduino.h>
#include <functional>
#include <vector>

/*
 * TaskManager
 *
 * Gestion centralisée des tâches périodiques non bloquantes.
 *
 * Chaque tâche possède :
 * - callback : fonction à exécuter
 * - intervalMs : période en millisecondes
 * - lastRunMs : timestamp de la dernière exécution
 *
 * Usage :
 *   TaskManager::init();
 *   TaskManager::addTask(callback, intervalMs);
 *   TaskManager::handle(); // à appeler dans loop()
 */

class TaskManager {
public:
    struct Task {
        std::function<void()> callback;  // Fonction à exécuter
        unsigned long intervalMs;        // Intervalle en ms
        unsigned long lastRunMs;         // Dernière exécution
    };

    // -------------------------------------------------------------------------
    // Initialisation / loop
    // -------------------------------------------------------------------------
    static void init();   // Initialise le gestionnaire
    static void handle(); // Appelé dans loop(), exécute les tâches prêtes

    // -------------------------------------------------------------------------
    // Gestion des tâches
    // -------------------------------------------------------------------------
    static void addTask(const std::function<void()>& callback, unsigned long intervalMs);
    static void clearTasks();  // Supprime toutes les tâches
};
