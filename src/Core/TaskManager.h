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
 * - lastRunMs : dernière échéance théorique honorée (pas l'instant réel)
 *
 * Usage :
 *   TaskManager::init();
 *   TaskManager::addTask(callback, intervalMs);
 *   TaskManager::handle(); // à appeler dans loop()
 *
 * Décalage de phase :
 *  - handle() exécute dans la MÊME passe de boucle toutes les tâches échues.
 *    Deux tâches de périodes harmoniques et de même origine tombent donc
 *    systématiquement ensemble, et leurs durées s'additionnent dans cette passe.
 *  - Le troisième paramètre de addTask() décale l'origine d'une tâche, ce qui
 *    sépare durablement sa grille de celle des autres. Voir TimingConfig.h,
 *    section « TaskManager — décalage de phase », pour les valeurs et le
 *    raisonnement de non-collision.
 *  - Une tâche enregistrée sans ce paramètre se comporte exactement comme
 *    avant : elle n'est pas décalée.
 */

class TaskManager {
public:
    struct Task {
        std::function<void()> callback;  // Fonction à exécuter
        unsigned long intervalMs;        // Intervalle en ms
        unsigned long lastRunMs;         // Dernière échéance théorique honorée
    };

    // -------------------------------------------------------------------------
    // Initialisation / loop
    // -------------------------------------------------------------------------
    static void init();   // Initialise le gestionnaire
    static void handle(); // Appelé dans loop(), exécute les tâches prêtes

    // -------------------------------------------------------------------------
    // Gestion des tâches
    // -------------------------------------------------------------------------
    // startOffsetMs décale l'origine de la grille de cette tâche :
    //  - 0 (défaut) : comportement historique, première exécution une période
    //    complète après le démarrage, comme toutes les autres tâches ;
    //  - non nul : première exécution startOffsetMs après cet enregistrement,
    //    puis toutes les intervalMs. Une tâche lente publie donc une valeur dès
    //    le démarrage au lieu d'attendre sa première période complète.
    // Un offset supérieur ou égal à la période est ramené dans la période : un
    // décalage n'a de sens qu'à l'intérieur de celle-ci.
    static void addTask(const std::function<void()>& callback,
                        unsigned long intervalMs,
                        unsigned long startOffsetMs = 0);

    static void clearTasks();  // Supprime toutes les tâches
};
