// Core/TaskManagerMonitor.cpp

#include "Core/TaskManagerMonitor.h"
#include "Config/TimingConfig.h"
#include "Storage/DataLogger.h"
#include "Connectivity/SmsManager.h"
#include "Utils/Console.h"

static const char* TAG = "TaskMon";

// -----------------------------------------------------------------------------
// Variables statiques
// -----------------------------------------------------------------------------

uint32_t TaskManagerMonitor::lastCheckMs = 0;
uint32_t TaskManagerMonitor::initTimeMs  = 0;
uint32_t TaskManagerMonitor::lastSmsMs   = 0;

// -----------------------------------------------------------------------------
// Initialisation
// -----------------------------------------------------------------------------

void TaskManagerMonitor::init()
{
    // lastCheckMs = 0 marque "pas encore de référence".
    // Le premier appel à checkSchedulerRegularity() servira de point de
    // référence et n'évaluera pas de delta.
    lastCheckMs = 0;

    // Point de départ de la période de grâce avant armement du SMS.
    initTimeMs  = millis();

    // Aucun SMS envoyé pour l'instant.
    lastSmsMs   = 0;

    Console::info(TAG, "Initialise");
}

// -----------------------------------------------------------------------------
// Tâche périodique — mesure sa propre régularité d'exécution
// -----------------------------------------------------------------------------

void TaskManagerMonitor::checkSchedulerRegularity()
{
    uint32_t now = millis();

    // Premier appel (ou init() jamais appelé) : on pose la référence
    // et on sort sans évaluer. Protège contre le faux positif au boot
    // et contre un appel anticipé avant init().
    if (lastCheckMs == 0) {
        lastCheckMs = now;
        return;
    }

    evaluateDelta(now);
    lastCheckMs = now;
}

// -----------------------------------------------------------------------------
// Évaluation de la dérive temporelle
// -----------------------------------------------------------------------------

void TaskManagerMonitor::evaluateDelta(uint32_t nowMs)
{
    uint32_t delta = nowMs - lastCheckMs;

    bool inRange =
        (delta >= TASKMON_MIN_ACCEPTABLE_PERIOD_MS) &&
        (delta <= TASKMON_MAX_ACCEPTABLE_PERIOD_MS);

    if (inRange) {
        return;
    }

    // ─── Dérive détectée — chaque occurrence est signalée ─────────────────

    // Console : message verbeux avec la valeur nominale attendue
    Console::info(TAG, "Periode d'execution non conforme : "
        + String(delta) + " ms au lieu de "
        + String(TASKMON_CHECK_PERIOD_MS) + " ms");

    // DataLogger / MQTT — canal métrique : valeur exploitable machine
    // (filtrage côté serveur, règles externes, graphes, corrélations)
    DataLogger::push(
        DataId::TaskMonPeriod,
        static_cast<float>(delta)
    );

    // ─── Alerte SMS ───────────────────────────────────────────────────────
    // Conditions cumulatives :
    //  1. Période de grâce écoulée depuis init()
    //  2. Aucun SMS envoyé OU cooldown écoulé depuis le dernier SMS

    bool gracePassed   = (nowMs - initTimeMs) >= TASKMON_SMS_GRACE_MS;
    bool cooldownReady = (lastSmsMs == 0) ||
                         ((nowMs - lastSmsMs) >= TASKMON_SMS_COOLDOWN_MS);

    if (gracePassed && cooldownReady) {
        SmsManager::alert(
            "Serre de Marie-Pierre : Periode systeme non conforme : "
            + String(delta) + " ms (attendu "
            + String(TASKMON_CHECK_PERIOD_MS) + " ms)"
        );
        lastSmsMs = nowMs;
    }
}