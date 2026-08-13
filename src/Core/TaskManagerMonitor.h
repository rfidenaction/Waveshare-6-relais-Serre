// Core/TaskManagerMonitor.h
#pragma once

#include <Arduino.h>

/*
 * TaskManagerMonitor
 *
 * Supervision de la régularité du scheduler (TaskManager).
 *
 * Principe :
 *  - Le monitor est enregistré comme une tâche périodique normale auprès
 *    du TaskManager, avec la période TASKMON_CHECK_PERIOD_MS.
 *  - À chaque exécution, checkSchedulerRegularity() mesure le delta temporel
 *    entre l'appel courant et l'appel précédent.
 *
 * Indépendance :
 *  - Aucun couplage avec un module métier (EventManager, etc.).
 *  - C'est la propre régularité d'exécution du monitor qui sert de référence.
 *  - Si le scheduler ralentit ou se bloque, la tâche n'est plus appelée à
 *    l'heure et la dérive est immédiatement détectée.
 *
 * Publication — synthèse périodique, jamais l'événement :
 *  - Le monitor retient sur une fenêtre la pire période mesurée, c'est-à-dire
 *    l'échantillon dont l'écart à TASKMON_CHECK_PERIOD_MS est le plus grand.
 *    Le sens de la dérive est conservé : un retard remonte au-dessus de la
 *    période nominale, une avance au-dessous.
 *  - À l'échéance, cette valeur est publiée sur DataId::TaskMonPeriod, sans
 *    aucun filtrage ni seuil : une fenêtre parfaitement nominale publie la
 *    période nominale. L'interface reste donc toujours à jour, et une valeur
 *    qui cesse d'être rafraîchie devient elle-même un signal.
 *  - La première publication a lieu à BOOT_VERDICT_DELAY_MS et couvre la
 *    séquence de démarrage ; les suivantes toutes les TASKMON_REPORT_PERIOD_MS.
 *  - La valeur publiée est écrêtée à PUBLISH_CLAMP_MS pour rester dans les
 *    bornes META : au-delà, DataBus rejetterait le record exactement dans le
 *    cas le plus grave. La console et le SMS conservent la valeur brute.
 *
 * Alerte SMS :
 *  - Après une période de grâce SMS_TASKMON_GRACE_MS suivant init(), toute
 *    période sortant de la plage [SMS_TASKMON_MIN_PERIOD_MS ;
 *    SMS_TASKMON_MAX_PERIOD_MS] arme un SMS d'alerte (pas d'envoi immédiat).
 *    Ces seuils ne conditionnent QUE le SMS : ils n'ont aucun effet sur la
 *    valeur publiée.
 *  - L'envoi est différé de SMS_TASKMON_BEFORE_SENDING_MS après l'armement.
 *    Ce délai protège contre l'amplification d'une panne : l'envoi d'un SMS
 *    coupe temporairement internet (bascule PPP → COMMAND MODE sur la LilyGo),
 *    et un SMS déclenché trop tôt sur une dérive transitoire aggraverait
 *    la situation au lieu de l'alerter.
 *  - La valeur du delta qui a armé le SMS est mémorisée jusqu'à l'envoi,
 *    pour être incluse dans le message.
 *  - Un cooldown SMS_TASKMON_COOLDOWN_MS empêche le harcèlement en cas
 *    de problème durable. L'information reste disponible via MQTT.
 *
 * IMPORTANT :
 *  - aucun correctif automatique
 *  - aucune action bloquante
 *  - tous les timings viennent de TimingConfig.h et SmsManager.h
 *  - pas d'état latché pour le SMS : chaque dérive est traitée individuellement
 */

class TaskManagerMonitor {
public:
    // Initialisation (à appeler une fois dans loopInit())
    static void init();

    // Tâche périodique enregistrée auprès du TaskManager.
    // Mesure sa propre régularité d'exécution et signale toute dérive.
    static void checkSchedulerRegularity();

private:
    // Plafond de la valeur publiée, imposé par la borne haute de META
    // (DataId::TaskMonPeriod, max 10000 ms). Une période au-delà est publiée
    // à cette valeur sentinelle plutôt que rejetée par DataBus.
    static constexpr uint32_t PUBLISH_CLAMP_MS = 9999;

    // Seuils de la trace console, volontairement en dur et indépendants des
    // seuils SMS : ils ne servent qu'à ne pas noyer la console au rythme d'une
    // ligne toutes les TASKMON_CHECK_PERIOD_MS.
    static constexpr uint32_t CONSOLE_MIN_PERIOD_MS = 1990;
    static constexpr uint32_t CONSOLE_MAX_PERIOD_MS = 2010;

    static uint32_t lastCheckMs;
    static uint32_t initTimeMs;
    static uint32_t lastSmsMs;
    static uint32_t pendingSmsAtMs;   // 0 = aucun SMS armé, sinon instant d'envoi prévu
    static uint32_t pendingSmsDelta;  // Valeur de la dérive qui a armé le SMS (ms)

    // ─── Fenêtre courante de la synthèse périodique ──────────────────────
    static uint32_t worstDelta;       // Pire période de la fenêtre (ms)
    static uint32_t nextReportMs;     // Échéance de la prochaine publication
    static bool     reportStarted;    // La première synthèse est partie

    static void evaluateDelta(uint32_t nowMs);

    // Publie la pire période de la fenêtre écoulée et rouvre une fenêtre.
    static void publishReport();
};