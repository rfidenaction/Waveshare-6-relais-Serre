// src/Gardener/ConditionalWatering.cpp
//
// Arrosage conditionnel — voir ConditionalWatering.h

#include "Gardener/ConditionalWatering.h"
#include "Connectivity/MqttManager.h"
#include "Core/DataBus.h"
#include "Core/VirtualClock.h"
#include "Config/IO-Config.h"
#include "Utils/Console.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <math.h>
#include <time.h>

static const char* TAG = "Conditional";

// ─── Variables statiques ─────────────────────────────────────────────────────

ConditionalRule ConditionalWatering::conditionalRules[MAX_CONDITIONAL_RULES];
uint8_t         ConditionalWatering::conditionalRuleCount = 0;
uint32_t        ConditionalWatering::conditionalRuleLastTrigger[MAX_CONDITIONAL_RULES] = {};

ConditionalWatering::PendingMeasure
             ConditionalWatering::pendingMeasures[MAX_PENDING_MEASURES] = {};
uint8_t      ConditionalWatering::pendingMeasureCount = 0;
portMUX_TYPE ConditionalWatering::pendingMeasureMux   = portMUX_INITIALIZER_UNLOCKED;

char          ConditionalWatering::conditionalMsgBuffer[MSG_BUFFER_SIZE] = {};
volatile bool ConditionalWatering::conditionalMsgPending = false;
portMUX_TYPE  ConditionalWatering::conditionalMsgMux = portMUX_INITIALIZER_UNLOCKED;
char          ConditionalWatering::conditionalMsgWork[MSG_BUFFER_SIZE] = {};

volatile bool ConditionalWatering::conditionalStatePublishPending = false;

// ─── init() ──────────────────────────────────────────────────────────────────

void ConditionalWatering::init()
{
    loadConditionalRules();
    Console::info(TAG, "Arrosage conditionnel démarré — "
                  + String(conditionalRuleCount) + " règle(s) chargée(s)");
}

// ─── onNewData() — thread du producteur ──────────────────────────────────────
// Appelée par DataBus::distribute() pour CHAQUE donnée publiée. Écarte ce qui
// n'est pas une mesure de capteur numérique, puis délègue.

void ConditionalWatering::onNewData(const BusItem& item)
{
    if (item.type != DataType::Sensor)  return;
    if (item.valueKind != 0)            return;

    offerMeasure(item.id, item.valueFloat);
}

// ─── offerMeasure() — thread du producteur ───────────────────────────────────
// Entrée unique des mesures, qu'elles viennent du bus (via onNewData) ou
// directement d'un module capteur qui a lu sans publier. Doit rester minimale :
// filtrage, puis mémorisation de la mesure sous portMUX.
//
// Les contrôles META reproduisent ceux de DataBus::validate(). Sans eux, la
// voie directe accepterait ce que le bus rejette : une trame corrompue de CRC
// valide pourrait ouvrir une vanne, puis la publication de la mesure serait
// refusée pour dépassement de bornes — l'action orpheline que ce module a
// précisément pour rôle d'interdire.
//
// Le parcours de conditionalRules[] se fait sans verrou alors que handle()
// peut modifier le tableau. C'est sans conséquence : au pire on mémorise une
// mesure qui ne servira à personne, ou on en ignore une, à un tick près.

void ConditionalWatering::offerMeasure(DataId sensorId, float value)
{
    if (conditionalRuleCount == 0)         return;
    if (!isValidId((uint8_t)sensorId))     return;

    const DataMeta& meta = getMeta(sensorId);
    if (meta.type   != DataType::Sensor)     return;
    if (meta.nature != DataNature::metrique) return;

    if (value < meta.min || value > meta.max) {
        Console::warn(TAG, "Mesure écartée — " + String(meta.label)
                      + " = " + String(value, 1) + " " + String(meta.unit)
                      + " hors bornes META [" + String(meta.min, 1)
                      + ", " + String(meta.max, 1) + "]");
        return;
    }

    bool sensorUsed = false;
    for (uint8_t i = 0; i < conditionalRuleCount; i++) {
        if (conditionalRules[i].sensorId == sensorId) { sensorUsed = true; break; }
    }
    if (!sensorUsed) return;

    taskENTER_CRITICAL(&pendingMeasureMux);

    bool stored = false;
    for (uint8_t i = 0; i < pendingMeasureCount; i++) {
        if (pendingMeasures[i].sensorId == sensorId) {
            pendingMeasures[i].value = value;
            stored = true;
            break;
        }
    }
    if (!stored && pendingMeasureCount < MAX_PENDING_MEASURES) {
        pendingMeasures[pendingMeasureCount].sensorId = sensorId;
        pendingMeasures[pendingMeasureCount].value    = value;
        pendingMeasureCount++;
    }

    taskEXIT_CRITICAL(&pendingMeasureMux);
}

// ─── handle() — tâche périodique ─────────────────────────────────────────────

void ConditionalWatering::handle()
{
    // 1. Publication d'état demandée depuis le thread esp_mqtt.
    if (conditionalStatePublishPending) {
        conditionalStatePublishPending = false;
        publishConditionalState();
    }

    // 2. Traiter le buffer MQTT entrant (FromUser) si présent. Indépendant de
    //    l'horloge : l'utilisateur doit pouvoir configurer ses règles même
    //    avant que VClock ne soit disponible.
    //    La recopie sous portMUX est indispensable : le parsing ArduinoJson
    //    est en zéro-copie et relit le buffer bien après la désérialisation.
    bool hasMsg = false;

    taskENTER_CRITICAL(&conditionalMsgMux);
    if (conditionalMsgPending) {
        memcpy(conditionalMsgWork, conditionalMsgBuffer, MSG_BUFFER_SIZE);
        conditionalMsgPending = false;
        hasMsg = true;
    }
    taskEXIT_CRITICAL(&conditionalMsgMux);

    if (hasMsg) {
        processConditionalMessage(conditionalMsgWork);
    }

    // 3. Extraire les mesures reçues depuis le tick précédent.
    PendingMeasure measures[MAX_PENDING_MEASURES];
    uint8_t measureCount;

    taskENTER_CRITICAL(&pendingMeasureMux);
    measureCount = pendingMeasureCount;
    for (uint8_t i = 0; i < measureCount; i++) {
        measures[i] = pendingMeasures[i];
    }
    pendingMeasureCount = 0;
    taskEXIT_CRITICAL(&pendingMeasureMux);

    if (measureCount == 0) return;

    // 4. Sans horloge, la plage horaire n'a pas de sens : mesures abandonnées.
    TimeVClock t = VirtualClock::read();
    if (!t.VClock_available) return;

    time_t ts = (time_t)t.timestamp;
    struct tm tmLocal;
    localtime_r(&ts, &tmLocal);

    // 5. Évaluer les règles concernées par chaque mesure.
    for (uint8_t i = 0; i < measureCount; i++) {
        evaluateRulesForSensor(measures[i].sensorId, measures[i].value,
                               (uint32_t)t.timestamp, (uint8_t)tmLocal.tm_hour);
    }
}

// ─── evaluateRulesForSensor() ────────────────────────────────────────────────
// Trois filtres successifs : plage horaire, condition, délai de repos.
//
// Au premier déclenchement effectif, la mesure qui a décidé est publiée à son
// tour (voir ConditionalWatering.h, « pas d'action orpheline »). Une seule fois
// par appel : deux règles peuvent partager le même capteur.

void ConditionalWatering::evaluateRulesForSensor(DataId sensorId, float value,
                                                 uint32_t nowTs, uint8_t localHour)
{
    bool measurePublished = false;

    for (uint8_t i = 0; i < conditionalRuleCount; i++) {
        const ConditionalRule& rule = conditionalRules[i];
        if (rule.sensorId != sensorId) continue;

        // Plage horaire. endHour < startHour décrit une fenêtre qui franchit
        // minuit ; l'heure de fin est toujours la dernière heure incluse.
        bool inWindow = (rule.startHour <= rule.endHour)
                      ? (localHour >= rule.startHour && localHour <= rule.endHour)
                      : (localHour >= rule.startHour || localHour <= rule.endHour);
        if (!inWindow) continue;

        // Condition
        bool conditionMet = (rule.cmp == CMP_LOWER)
                          ? (value <  rule.threshold)
                          : (value >= rule.threshold);
        if (!conditionMet) continue;

        // Délai de repos. Un recul de l'horloge (resynchronisation) est traité
        // comme un repos écoulé plutôt que comme une attente sans fin.
        uint32_t lastTrigger = conditionalRuleLastTrigger[i];
        if (lastTrigger != 0 && nowTs >= lastTrigger) {
            uint32_t restSeconds = (uint32_t)rule.restHours * 3600UL;
            if ((nowTs - lastTrigger) < restSeconds) {
                Console::info(TAG, "Condition remplie mais repos en cours — "
                              + String(getMeta(rule.sensorId).label)
                              + " = " + String(value, 1)
                              + " " + String(getMeta(rule.sensorId).unit));
                continue;
            }
        }

        // Déclenchement. Si la vanne est déjà ouverte, ValveManager ignorera la
        // demande : le repos démarre quand même, le travail étant fait.
        BusItem item = {};
        item.type       = DataType::CommandConditional;
        item.id         = rule.cmdId;
        item.valueKind  = 0;
        item.valueFloat = (float)rule.duration;

        // Si la commande n'a pas pu être routée, aucun arrosage n'aura lieu :
        // armer le repos condamnerait la règle au silence pendant restHours.
        // Cas courant au boot, ValveManager n'acceptant rien avant
        // VALVE_START_DELAY_MS. On retente donc à la prochaine mesure.
        if (!DataBus::publish(item)) {
            Console::warn(TAG, "Déclenchement non routé cmdId="
                          + String((uint8_t)rule.cmdId)
                          + " — repos non armé, nouvelle tentative à la"
                            " prochaine mesure");
            continue;
        }

        conditionalRuleLastTrigger[i] = nowTs;

        Console::info(TAG, "Arrosage conditionnel cmdId="
                      + String((uint8_t)rule.cmdId)
                      + " — " + String(getMeta(rule.sensorId).label)
                      + " = " + String(value, 1)
                      + " " + String(getMeta(rule.sensorId).unit)
                      + (rule.cmp == CMP_LOWER ? " < " : " >= ")
                      + String(rule.threshold, 1)
                      + " — durée " + String(rule.duration)
                      + " s, repos " + String(rule.restHours) + " h");

        // Publication de la mesure qui a décidé. Le repos est déjà armé, donc
        // la réévaluation que cette publication provoque (distribute →
        // onNewData → tick suivant) sera écartée par le filtre de repos.
        if (!measurePublished) {
            BusItem measure = {};
            measure.type       = getMeta(sensorId).type;
            measure.id         = sensorId;
            measure.valueKind  = 0;
            measure.valueFloat = value;
            DataBus::publish(measure);
            measurePublished = true;
        }
    }
}

// ─── onConditionalMessage() — thread esp_mqtt ────────────────────────────────
// Bufferise le message brut pour traitement dans handle() (thread TaskManager).
// Si un nouveau message arrive avant traitement, il écrase le précédent.

void ConditionalWatering::onConditionalMessage(const char* data, int len)
{
    if (len <= 0 || (size_t)len >= MSG_BUFFER_SIZE) {
        return;
    }
    taskENTER_CRITICAL(&conditionalMsgMux);
    memcpy(conditionalMsgBuffer, data, len);
    conditionalMsgBuffer[len] = '\0';
    conditionalMsgPending = true;
    taskEXIT_CRITICAL(&conditionalMsgMux);
}

// ─── requestStatePublish() — thread esp_mqtt ─────────────────────────────────
// Ne publie pas : la publication parcourt conditionalRules[] et doit rester
// dans le thread TaskManager. Voir handle().

void ConditionalWatering::requestStatePublish()
{
    conditionalStatePublishPending = true;
}

// ─── processConditionalMessage() — thread TaskManager ────────────────────────
// Parse le JSON FromUser et exécute l'opération add ou remove.
// Publie l'état courant (ToUser) dans tous les cas.
// msg est la copie de travail, jamais le buffer partagé avec le thread esp_mqtt.

void ConditionalWatering::processConditionalMessage(char* msg)
{
    StaticJsonDocument<MSG_BUFFER_SIZE> doc;
    DeserializationError err = deserializeJson(doc, msg);
    if (err) {
        Console::warn(TAG, "JSON FromUser malformé : " + String(err.c_str()));
        publishConditionalState();
        return;
    }

    const char* op = doc["op"] | "";

    if (strcmp(op, "add") == 0 || strcmp(op, "remove") == 0) {
        ConditionalRule rule;
        rule.cmdId     = (DataId)(uint8_t)(doc["cmdId"]    | 0);
        rule.sensorId  = (DataId)(uint8_t)(doc["sensorId"] | 0);
        rule.cmp       = doc["cmp"]       | 0;
        rule.threshold = doc["threshold"] | 0.0f;
        rule.startHour = doc["startHour"] | 0;
        rule.endHour   = doc["endHour"]   | 0;
        rule.restHours = doc["restHours"] | 0;
        rule.duration  = doc["duration"]  | 0;

        if (strcmp(op, "add") == 0) {
            addConditionalRule(rule);
        } else {
            removeConditionalRule(rule);
        }

    } else {
        Console::warn(TAG, "op inconnu : " + String(op));
    }

    publishConditionalState();
}

// ─── validateConditionalRule() ───────────────────────────────────────────────
// Toutes les bornes viennent de META et de RELAYS[], sources de vérité uniques.

bool ConditionalWatering::validateConditionalRule(const ConditionalRule& rule)
{
    // ── Vanne ────────────────────────────────────────────────────────────
    if (!isValidId((uint8_t)rule.cmdId)) {
        Console::warn(TAG, "cmdId=" + String((uint8_t)rule.cmdId) + " inconnu de META");
        return false;
    }
    const DataMeta& cmdMeta = getMeta(rule.cmdId);
    if (cmdMeta.type != DataType::CommandGeneric) {
        Console::warn(TAG, "cmdId=" + String((uint8_t)rule.cmdId)
                      + " n'est pas une commande (type="
                      + String((uint8_t)cmdMeta.type) + ")");
        return false;
    }

    bool found = false;
    for (size_t i = 0; i < RELAYS_COUNT; i++) {
        if (RELAYS[i].command == rule.cmdId) { found = true; break; }
    }
    if (!found) {
        Console::warn(TAG, "cmdId=" + String((uint8_t)rule.cmdId)
                      + " absent de RELAYS[]");
        return false;
    }

    if (rule.duration < (uint16_t)cmdMeta.min ||
        rule.duration > (uint16_t)cmdMeta.max) {
        Console::warn(TAG, "duration=" + String(rule.duration)
                      + " hors bornes META [" + String(cmdMeta.min, 0)
                      + ", " + String(cmdMeta.max, 0) + "]");
        return false;
    }

    // ── Capteur ──────────────────────────────────────────────────────────
    if (!isValidId((uint8_t)rule.sensorId)) {
        Console::warn(TAG, "sensorId=" + String((uint8_t)rule.sensorId)
                      + " inconnu de META");
        return false;
    }
    const DataMeta& sensorMeta = getMeta(rule.sensorId);
    if (sensorMeta.type != DataType::Sensor) {
        Console::warn(TAG, "sensorId=" + String((uint8_t)rule.sensorId)
                      + " n'est pas un capteur (type="
                      + String((uint8_t)sensorMeta.type) + ")");
        return false;
    }
    if (sensorMeta.nature != DataNature::metrique) {
        Console::warn(TAG, "sensorId=" + String((uint8_t)rule.sensorId)
                      + " n'est pas une grandeur métrique");
        return false;
    }
    if (rule.threshold < sensorMeta.min || rule.threshold > sensorMeta.max) {
        Console::warn(TAG, "threshold=" + String(rule.threshold, 1)
                      + " hors bornes META [" + String(sensorMeta.min, 1)
                      + ", " + String(sensorMeta.max, 1) + "]");
        return false;
    }

    // ── Comparateur, plage horaire, repos ────────────────────────────────
    if (rule.cmp != CMP_LOWER && rule.cmp != CMP_GREATER_OR_EQUAL) {
        Console::warn(TAG, "cmp=" + String(rule.cmp) + " invalide");
        return false;
    }
    if (rule.startHour > 23 || rule.endHour > 23) {
        Console::warn(TAG, "plage horaire " + String(rule.startHour)
                      + "h–" + String(rule.endHour) + "h hors bornes");
        return false;
    }
    if (rule.restHours < MIN_REST_HOURS || rule.restHours > MAX_REST_HOURS) {
        Console::warn(TAG, "restHours=" + String(rule.restHours)
                      + " hors bornes [" + String(MIN_REST_HOURS)
                      + ", " + String(MAX_REST_HOURS) + "]");
        return false;
    }

    return true;
}

// ─── sameConditionalRule() ───────────────────────────────────────────────────
// Une règle est identifiée par son contenu complet : c'est cette égalité qui
// sert à détecter les doublons et à retrouver la règle à supprimer.

bool ConditionalWatering::sameConditionalRule(const ConditionalRule& a,
                                              const ConditionalRule& b)
{
    return a.cmdId     == b.cmdId
        && a.sensorId  == b.sensorId
        && a.cmp       == b.cmp
        && a.startHour == b.startHour
        && a.endHour   == b.endHour
        && a.restHours == b.restHours
        && a.duration  == b.duration
        && fabsf(a.threshold - b.threshold) < 0.05f;
}

// ─── addConditionalRule() ────────────────────────────────────────────────────

bool ConditionalWatering::addConditionalRule(const ConditionalRule& rule)
{
    if (!validateConditionalRule(rule)) return false;

    if (conditionalRuleCount >= MAX_CONDITIONAL_RULES) {
        Console::warn(TAG, "Tableau plein (" + String(MAX_CONDITIONAL_RULES)
                      + " règles)");
        return false;
    }

    for (uint8_t i = 0; i < conditionalRuleCount; i++) {
        if (sameConditionalRule(conditionalRules[i], rule)) {
            Console::warn(TAG, "Doublon détecté : règle identique déjà présente");
            return false;
        }
    }

    conditionalRules[conditionalRuleCount]           = rule;
    conditionalRuleLastTrigger[conditionalRuleCount] = 0;
    conditionalRuleCount++;

    // Sauvegarde impossible : l'écriture atomique garantit que
    // /conditional.json est resté intact, donc on annule l'ajout en RAM. Sans
    // cela la règle vivrait jusqu'au prochain reboot et serait publiée sur
    // ToUser, laissant croire à l'utilisateur qu'elle est enregistrée.
    if (!saveConditionalRules()) {
        conditionalRuleCount--;
        Console::error(TAG, "Échec sauvegarde — règle NON ajoutée");
        return false;
    }

    Console::info(TAG, "Règle ajoutée : cmdId=" + String((uint8_t)rule.cmdId)
                  + " si " + String(getMeta(rule.sensorId).label)
                  + (rule.cmp == CMP_LOWER ? " < " : " >= ")
                  + String(rule.threshold, 1)
                  + " entre " + String(rule.startHour) + "h et "
                  + String(rule.endHour) + "h — durée "
                  + String(rule.duration) + " s, repos "
                  + String(rule.restHours) + " h");
    return true;
}

// ─── removeConditionalRule() ─────────────────────────────────────────────────

bool ConditionalWatering::removeConditionalRule(const ConditionalRule& rule)
{
    for (uint8_t i = 0; i < conditionalRuleCount; i++) {
        if (!sameConditionalRule(conditionalRules[i], rule)) continue;

        // Swap avec le dernier élément — les deux tableaux parallèles
        // doivent être déplacés ensemble. La règle retirée est mémorisée pour
        // pouvoir défaire l'opération si la sauvegarde échoue.
        const uint8_t         lastIdx     = conditionalRuleCount - 1;
        const ConditionalRule removedRule = conditionalRules[i];
        const uint32_t        removedTrig = conditionalRuleLastTrigger[i];

        conditionalRules[i]           = conditionalRules[lastIdx];
        conditionalRuleLastTrigger[i] = conditionalRuleLastTrigger[lastIdx];
        conditionalRuleCount--;

        // Même raisonnement que pour l'ajout : le fichier est intact, on
        // restaure l'état d'avant le swap pour que RAM et disque concordent.
        if (!saveConditionalRules()) {
            conditionalRuleCount++;
            conditionalRules[lastIdx]           = conditionalRules[i];
            conditionalRuleLastTrigger[lastIdx] = conditionalRuleLastTrigger[i];
            conditionalRules[i]                 = removedRule;
            conditionalRuleLastTrigger[i]       = removedTrig;
            Console::error(TAG, "Échec sauvegarde — règle NON supprimée");
            return false;
        }

        Console::info(TAG, "Règle supprimée : cmdId=" + String((uint8_t)rule.cmdId)
                      + " / sensorId=" + String((uint8_t)rule.sensorId));
        return true;
    }

    Console::warn(TAG, "Règle inexistante : cmdId=" + String((uint8_t)rule.cmdId)
                  + " / sensorId=" + String((uint8_t)rule.sensorId));
    return false;
}

// ─── serializeConditionalRules() ─────────────────────────────────────────────
// Format JSON commun à la persistance LittleFS et à la publication MQTT ToUser.

String ConditionalWatering::serializeConditionalRules()
{
    DynamicJsonDocument doc(4096);
    JsonArray rules = doc.createNestedArray("rules");

    for (uint8_t i = 0; i < conditionalRuleCount; i++) {
        const ConditionalRule& r = conditionalRules[i];
        JsonObject obj = rules.createNestedObject();
        obj["cmdId"]     = (uint8_t)r.cmdId;
        obj["sensorId"]  = (uint8_t)r.sensorId;
        obj["cmp"]       = r.cmp;
        obj["threshold"] = serialized(String(r.threshold, 1));
        obj["startHour"] = r.startHour;
        obj["endHour"]   = r.endHour;
        obj["restHours"] = r.restHours;
        obj["duration"]  = r.duration;
    }

    String result;
    serializeJson(doc, result);
    return result;
}

// ─── saveConditionalRules() ──────────────────────────────────────────────────
// Écriture atomique : /conditional.tmp → rename → /conditional.json.

bool ConditionalWatering::saveConditionalRules()
{
    String json = serializeConditionalRules();

    File f = LittleFS.open("/conditional.tmp", "w");
    if (!f) {
        Console::error(TAG, "Échec ouverture /conditional.tmp en écriture");
        return false;
    }

    size_t written = f.print(json);
    f.close();

    if (written != json.length()) {
        Console::error(TAG, "Écriture partielle /conditional.tmp ("
                      + String(written) + "/" + String(json.length()) + ")");
        return false;
    }

    if (!LittleFS.rename("/conditional.tmp", "/conditional.json")) {
        Console::error(TAG, "Échec rename /conditional.tmp → /conditional.json");
        return false;
    }

    return true;
}

// ─── loadConditionalRules() ──────────────────────────────────────────────────

bool ConditionalWatering::loadConditionalRules()
{
    conditionalRuleCount = 0;

    File f = LittleFS.open("/conditional.json", "r");
    if (!f) {
        Console::warn(TAG, "Fichier /conditional.json absent — démarrage à vide");
        return false;
    }

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Console::error(TAG, "JSON malformé dans /conditional.json : "
                      + String(err.c_str()));
        return false;
    }

    JsonArray rules = doc["rules"];
    for (JsonObject obj : rules) {
        if (conditionalRuleCount >= MAX_CONDITIONAL_RULES) {
            Console::warn(TAG, "Plus de " + String(MAX_CONDITIONAL_RULES)
                          + " règles dans le fichier — surplus ignoré");
            break;
        }

        ConditionalRule rule;
        rule.cmdId     = (DataId)(uint8_t)(obj["cmdId"]    | 0);
        rule.sensorId  = (DataId)(uint8_t)(obj["sensorId"] | 0);
        rule.cmp       = obj["cmp"]       | 0;
        rule.threshold = obj["threshold"] | 0.0f;
        rule.startHour = obj["startHour"] | 0;
        rule.endHour   = obj["endHour"]   | 0;
        rule.restHours = obj["restHours"] | 0;
        rule.duration  = obj["duration"]  | 0;

        if (validateConditionalRule(rule)) {
            conditionalRules[conditionalRuleCount]           = rule;
            conditionalRuleLastTrigger[conditionalRuleCount] = 0;
            conditionalRuleCount++;
        } else {
            Console::warn(TAG, "Règle ignorée au chargement (invalide)");
        }
    }

    return true;
}

// ─── publishConditionalState() ───────────────────────────────────────────────
// Sérialise l'état courant et publie via MqttManager (retain sur ToUser).

void ConditionalWatering::publishConditionalState()
{
    String json = serializeConditionalRules();
    MqttManager::publishConditionalState(json.c_str(), json.length());
}
