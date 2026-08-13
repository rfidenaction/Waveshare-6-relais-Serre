// Core/TaskManagerMonitor.cpp

#include "Core/TaskManagerMonitor.h"
#include "Config/TimingConfig.h"
#include "Core/DataBus.h"
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

uint32_t TaskManagerMonitor::worstDelta    = TASKMON_CHECK_PERIOD_MS;
uint32_t TaskManagerMonitor::nextReportMs  = 0;
bool     TaskManagerMonitor::reportStarted = false;

// Écart absolu d'une période mesurée à la période nominale.
static inline uint32_t deviation(uint32_t periodMs)
{
    return (periodMs > TASKMON_CHECK_PERIOD_MS)
         ? (periodMs - TASKMON_CHECK_PERIOD_MS)
         : (TASKMON_CHECK_PERIOD_MS - periodMs);
}

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

    // Fenêtre de synthèse ouverte sur la période nominale : écart nul par
    // construction, donc le premier échantillon mesuré la remplacera dès
    // qu'il s'en écarte. Une fenêtre sans aucun échantillon publie le nominal.
    worstDelta    = TASKMON_CHECK_PERIOD_MS;
    nextReportMs  = 0;
    reportStarted = false;

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
    // Étape 2 : mesure de la période et mise à jour de la fenêtre
    //
    // Seul l'échantillon le plus éloigné de la période nominale est retenu.
    // La comparaison porte sur l'écart absolu, mais c'est la période réelle
    // qui est mémorisée : le sens de la dérive est ainsi conservé jusqu'à
    // la publication.
    // -------------------------------------------------------------------------
    uint32_t delta = nowMs - lastCheckMs;

    if (deviation(delta) > deviation(worstDelta)) {
        worstDelta = delta;
    }

    // -------------------------------------------------------------------------
    // Étape 3 : trace console
    //
    // Seuils en dur : ils ne servent qu'à éviter une ligne toutes les deux
    // secondes. Ils n'ont aucun rôle fonctionnel — ni sur la valeur publiée,
    // ni sur l'alerte SMS.
    // -------------------------------------------------------------------------
    if (delta < CONSOLE_MIN_PERIOD_MS || delta > CONSOLE_MAX_PERIOD_MS) {
        Console::info(TAG, "Periode d'execution : "
            + String(delta) + " ms au lieu de "
            + String(TASKMON_CHECK_PERIOD_MS) + " ms");
    }

    // -------------------------------------------------------------------------
    // Étape 4 : échéance de la synthèse périodique
    //
    // Évaluée à chaque appel, donc avant tout court-circuit lié à la détection
    // de dérive : la publication est inconditionnelle, y compris quand tout est
    // nominal. La première synthèse part à BOOT_VERDICT_DELAY_MS et couvre la
    // séquence de démarrage.
    // -------------------------------------------------------------------------
    if (!reportStarted) {
        if (nowMs >= BOOT_VERDICT_DELAY_MS) {
            reportStarted = true;
            nextReportMs  = nowMs;
        }
    }

    // Soustraction signée : insensible au débordement de millis().
    if (reportStarted && (int32_t)(nowMs - nextReportMs) >= 0) {
        nextReportMs = nowMs + TASKMON_REPORT_PERIOD_MS;
        publishReport();
    }

    // -------------------------------------------------------------------------
    // Étape 5 : détection de dérive pour l'alerte SMS
    //
    // Plage volontairement large, réglée dans SmsManager.h : elle ne filtre
    // que l'envoi de SMS. Une dérive plus fine reste visible sur MQTT via la
    // synthèse périodique.
    // -------------------------------------------------------------------------
    bool inRange =
        (delta >= SMS_TASKMON_MIN_PERIOD_MS) &&
        (delta <= SMS_TASKMON_MAX_PERIOD_MS);

    if (inRange) {
        return;
    }

    // -------------------------------------------------------------------------
    // Étape 6 : armement du SMS (différé)
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

// -----------------------------------------------------------------------------
// Synthèse périodique — pire période de la fenêtre écoulée
// -----------------------------------------------------------------------------

void TaskManagerMonitor::publishReport()
{
    uint32_t worst = worstDelta;

    // La fenêtre est rouverte avant même de publier : c'est un instantané,
    // il ne se rejoue pas, y compris si DataBus rejette le record.
    worstDelta = TASKMON_CHECK_PERIOD_MS;

    // Écrêtage : au-delà de la borne haute de META, DataBus rejetterait le
    // record. La valeur sentinelle dit "au moins PUBLISH_CLAMP_MS".
    uint32_t published = (worst > PUBLISH_CLAMP_MS) ? PUBLISH_CLAMP_MS : worst;

    BusItem item = {};
    item.type       = getMeta(DataId::TaskMonPeriod).type;
    item.id         = DataId::TaskMonPeriod;
    item.valueKind  = 0;
    item.valueFloat = static_cast<float>(published);
    DataBus::publish(item);

    Console::info(TAG, "Synthese : pire periode "
        + String(worst) + " ms sur la fenetre ecoulee");
}