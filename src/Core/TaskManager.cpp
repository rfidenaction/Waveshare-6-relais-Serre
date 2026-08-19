#include "Core/TaskManager.h"

// Stockage interne des tâches
static std::vector<TaskManager::Task> tasks;

void TaskManager::init() {
    tasks.clear();
}

void TaskManager::handle() {
    unsigned long now = millis();
    for (auto& t : tasks) {
        if (now - t.lastRunMs >= t.intervalMs) {
            t.callback();
            // Dernier point de grille <= now : origine préservée (décalage
            // de phase), un seul appel par passe, pas de rattrapage en rafale
            // au démarrage. Garde intervalMs != 0 : division.
            if (t.intervalMs != 0) {
                unsigned long elapsed = now - t.lastRunMs;
                t.lastRunMs += t.intervalMs * (elapsed / t.intervalMs);
            }
        }
    }
}

void TaskManager::addTask(const std::function<void()>& callback,
                          unsigned long intervalMs,
                          unsigned long startOffsetMs) {
    Task t;
    t.callback = callback;
    t.intervalMs = intervalMs;

    if (intervalMs == 0) {
        t.lastRunMs = 0;
    } else if (startOffsetMs == 0) {
        t.lastRunMs = millis();
    } else {
        // Un décalage n'a de sens qu'à l'intérieur de la période : au-delà, la
        // tâche serait due en permanence (voir la soustraction ci-dessous).
        startOffsetMs %= intervalMs;

        // Rendre la tâche due à startOffsetMs après cet instant : la différence
        // now - lastRunMs vaut alors intervalMs pile à cette échéance.
        // L'arithmétique non signée est modulaire, donc le passage « sous zéro »
        // de cette soustraction est sans conséquence — c'est la même propriété
        // qui rend handle() insensible au débordement de millis().
        t.lastRunMs = millis() - intervalMs + startOffsetMs;
    }

    tasks.push_back(t);
}

void TaskManager::clearTasks() {
    tasks.clear();
}
