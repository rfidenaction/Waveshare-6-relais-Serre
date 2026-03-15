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
            t.lastRunMs = now;
        }
    }
}

void TaskManager::addTask(const std::function<void()>& callback, unsigned long intervalMs) {
    Task t;
    t.callback = callback;
    t.intervalMs = intervalMs;
    t.lastRunMs = 0;
    tasks.push_back(t);
}

void TaskManager::clearTasks() {
    tasks.clear();
}
