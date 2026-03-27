// Core/TaskManagerMonitor.cpp

#include "Core/TaskManagerMonitor.h"
#include "Config/TimingConfig.h"
#include "Storage/DataLogger.h"
#include "Utils/Console.h"

static const char* TAG = "TaskMon";

// -----------------------------------------------------------------------------
// Variables statiques
// -----------------------------------------------------------------------------

uint32_t TaskManagerMonitor::lastCallMs         = 0;
bool     TaskManagerMonitor::warningActive      = false;
uint32_t TaskManagerMonitor::warningTimestampMs = 0;

// -----------------------------------------------------------------------------
// Initialisation
// -----------------------------------------------------------------------------

void TaskManagerMonitor::init()
{
    lastCallMs         = millis();
    warningActive      = false;
    warningTimestampMs = 0;
}

// -----------------------------------------------------------------------------
// Notification d’appel (appelée par EventManager)
// -----------------------------------------------------------------------------

void TaskManagerMonitor::notifyCall()
{
    uint32_t now = millis();
    evaluateDelta(now);
    lastCallMs = now;
}

// -----------------------------------------------------------------------------
// Évaluation de la dérive temporelle
// -----------------------------------------------------------------------------

void TaskManagerMonitor::evaluateDelta(uint32_t nowMs)
{
    if (lastCallMs == 0) {
        return;
    }

    uint32_t delta = nowMs - lastCallMs;

    bool inRange =
        (delta >= EVENT_MANAGER_MIN_PERIOD_MS) &&
        (delta <= EVENT_MANAGER_MAX_PERIOD_MS);

    // Passage OK → WARNING (latched)
    if (!inRange && !warningActive) {
        warningActive      = true;
        warningTimestampMs = nowMs;

        Console::info(TAG, "Delta hors plage : " + String(delta)
            + " ms (attendu " + String(EVENT_MANAGER_MIN_PERIOD_MS)
            + "-" + String(EVENT_MANAGER_MAX_PERIOD_MS) + " ms)");

        // Enregistrement événement système
        DataLogger::push(
            DataId::Error,          // volontairement générique pour l’instant
            static_cast<float>(delta)
        );
    }
}

// -----------------------------------------------------------------------------
// Accesseurs état
// -----------------------------------------------------------------------------

bool TaskManagerMonitor::isWarningActive()
{
    return warningActive;
}

uint32_t TaskManagerMonitor::getWarningTimestampMs()
{
    return warningTimestampMs;
}

// -----------------------------------------------------------------------------
// Acquittement utilisateur
// -----------------------------------------------------------------------------

void TaskManagerMonitor::acknowledgeWarning()
{
    warningActive      = false;
    warningTimestampMs = 0;
}