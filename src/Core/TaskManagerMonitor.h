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
 *  - Si ce delta sort de la plage [TASKMON_MIN_ACCEPTABLE_PERIOD_MS ;
 *    TASKMON_MAX_ACCEPTABLE_PERIOD_MS], une dérive est signalée.
 *
 * Indépendance :
 *  - Aucun couplage avec un module métier (EventManager, etc.).
 *  - C'est la propre régularité d'exécution du monitor qui sert de référence.
 *  - Si le scheduler ralentit ou se bloque, la tâche n'est plus appelée à
 *    l'heure et la dérive est immédiatement détectée.
 *
 * Alerte SMS :
 *  - Après une période de grâce TASKMON_SMS_GRACE_MS suivant init(),
 *    toute dérive arme un SMS d'alerte (pas d'envoi immédiat).
 *  - L'envoi est différé de SMS_TASKMON_BEFORE_SENDING_MS après l'armement.
 *    Ce délai protège contre l'amplification d'une panne : l'envoi d'un SMS
 *    coupe temporairement internet (bascule PPP → COMMAND MODE sur la LilyGo),
 *    et un SMS déclenché trop tôt sur une dérive transitoire aggraverait
 *    la situation au lieu de l'alerter.
 *  - La valeur du delta qui a armé le SMS est mémorisée jusqu'à l'envoi,
 *    pour être incluse dans le message.
 *  - Un cooldown TASKMON_SMS_COOLDOWN_MS empêche le harcèlement en cas
 *    de problème durable. L'information reste disponible via MQTT.
 *
 * IMPORTANT :
 *  - aucun correctif automatique
 *  - aucune action bloquante
 *  - tous les timings viennent de TimingConfig.h et SmsManager.h
 *  - pas d'état latché : chaque dérive est traitée individuellement
 */

class TaskManagerMonitor {
public:
    // Initialisation (à appeler une fois dans loopInit())
    static void init();

    // Tâche périodique enregistrée auprès du TaskManager.
    // Mesure sa propre régularité d'exécution et signale toute dérive.
    static void checkSchedulerRegularity();

private:
    static uint32_t lastCheckMs;
    static uint32_t initTimeMs;
    static uint32_t lastSmsMs;
    static uint32_t pendingSmsAtMs;   // 0 = aucun SMS armé, sinon instant d'envoi prévu
    static uint32_t pendingSmsDelta;  // Valeur de la dérive qui a armé le SMS (ms)

    static void evaluateDelta(uint32_t nowMs);
};