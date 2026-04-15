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

uint32_t TaskManagerMonitor::lastCheckMs     = 0;
uint32_t TaskManagerMonitor::initTimeMs      = 0;
uint32_t TaskManagerMonitor::lastSmsMs       = 0;
uint32_t TaskManagerMonitor::pendingSmsAtMs  = 0;
uint32_t TaskManagerMonitor::pendingSmsDelta = 0;

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

    // Aucun SMS armé pour l'instant.
    pendingSmsAtMs  = 0;
    pendingSmsDelta = 0;

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
    // -------------------------------------------------------------------------
    // Étape 1 : envoi différé d'un SMS déjà armé
    //
    // Exécuté à chaque appel, indépendamment de l'existence ou non d'une
    // dérive à cet instant. Placé AVANT la détection pour qu'une nouvelle
    // dérive concomitante ne court-circuite pas le délai d'armement.
    //
    // Le message inclut la valeur de la dérive qui a armé le SMS
    // (mémorisée au moment de l'armement, pas celle de l'instant d'envoi).
    // -------------------------------------------------------------------------
    if (pendingSmsAtMs != 0 && nowMs >= pendingSmsAtMs) {
        SmsManager::alert(
            "Serre de Marie-Pierre : Periode systeme non conforme : "
            + String(pendingSmsDelta) + " ms (attendu "
            + String(TASKMON_CHECK_PERIOD_MS) + " ms)"
        );
        lastSmsMs       = nowMs;
        pendingSmsAtMs  = 0;
        pendingSmsDelta = 0;
    }

    // -------------------------------------------------------------------------
    // Étape 2 : détection de dérive
    // -------------------------------------------------------------------------
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

    // -------------------------------------------------------------------------
    // Étape 3 : armement du SMS (différé)
    //
    // Conditions cumulatives :
    //  1. Période de grâce écoulée depuis init()
    //  2. Aucun SMS envoyé OU cooldown écoulé depuis le dernier SMS
    //  3. Aucun SMS déjà armé (évite de repousser l'échéance à chaque dérive)
    //
    // Le delta courant est mémorisé dans pendingSmsDelta pour être inclus
    // dans le message SMS envoyé à l'étape 1 d'un appel ultérieur,
    // environ SMS_TASKMON_BEFORE_SENDING_MS plus tard (précision ± 1 tick
    // de TASKMON_CHECK_PERIOD_MS).
    // -------------------------------------------------------------------------
    bool gracePassed   = (nowMs - initTimeMs) >= SMS_TASKMON_GRACE_MS;
    bool cooldownReady = (lastSmsMs == 0) ||
                         ((nowMs - lastSmsMs) >= SMS_TASKMON_COOLDOWN_MS);
    bool notArmedYet   = (pendingSmsAtMs == 0);

    if (gracePassed && cooldownReady && notArmedYet) {
        if (!SMS_TASKMON_ENABLED) {
            Console::info(TAG,
                "Alerte SMS desactivee (SMS_TASKMON_ENABLED=false) - delta:"
                + String(delta) + " ms");
        } else {
            pendingSmsAtMs  = nowMs + SMS_TASKMON_BEFORE_SENDING_MS;
            pendingSmsDelta = delta;
            Console::info(TAG,
                "SMS d'alerte arme — envoi differe de "
                + String(SMS_TASKMON_BEFORE_SENDING_MS / 1000) + "s "
                "(delta: " + String(delta) + " ms)");
        }
    }
}