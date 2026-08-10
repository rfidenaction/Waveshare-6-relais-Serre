// src/Gardener/ConditionalWatering.h
//
// Arrosage conditionnel — déclenchement d'une vanne sur critère de capteur.
//
// Une règle associe une vanne, un capteur, un comparateur, un seuil, une
// plage horaire (heure locale, franchissement de minuit autorisé), un délai
// de repos en heures et une durée d'arrosage. 16 règles au maximum.
//
// Ce module ne cadence RIEN : c'est l'arrivée d'une mesure sur le bus qui
// déclenche l'évaluation. DataBus::distribute() appelle onNewData(), qui se
// contente de mémoriser la mesure ; la décision est prise au tick suivant de
// handle(), dans le thread TaskManager. Cette indirection est indispensable :
// décider dans onNewData() reviendrait à appeler DataBus::publish() depuis
// l'intérieur de DataBus::distribute().
//
// Comme on ne décide que sur une mesure qui vient d'être produite, aucun
// contrôle de fraîcheur n'est nécessaire.
//
// Au déclenchement, un BusItem (DataType::CommandConditional) est publié via
// DataBus::publish(), ce qui enclenche la chaîne existante (validation META,
// horodatage, distribution mqttQueue/logQueue/WebServer, routage via RELAYS[]
// → ValveManager). Si la vanne est déjà ouverte, ValveManager ignore la
// demande : le délai de repos démarre malgré tout, le travail étant fait.
//
// Le délai de repos vit en RAM seule et se perd au reboot (choix assumé).
//
// Une règle ne se modifie pas : elle s'identifie par son contenu complet, et
// la suppression consiste à renvoyer la règle entière.
//
// Intégration :
//   - init() appelé dans loopInit() après MqttManager::init()
//   - handle() en tâche TaskManager période CONDITIONAL_HANDLE_PERIOD_MS
//   - onNewData() appelé par DataBus::distribute() (n'importe quel thread)
//   - onConditionalMessage() appelé par MqttManager depuis le thread esp_mqtt
//   - publishConditionalState() appelé par MqttManager sur MQTT_EVENT_CONNECTED
#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "Config/MetaDataModel.h"

struct BusItem;  // forward declaration (défini dans Core/DataBus.h)

// ═════════════════════════════════════════════════════════════════════════════
// ConditionalRule — une règle d'arrosage conditionnel
//
// endHour < startHour décrit une fenêtre qui franchit minuit (ex. 22 → 5).
// ═════════════════════════════════════════════════════════════════════════════

struct ConditionalRule {
    DataId   cmdId;      // CommandValve1..CommandValve6 (ids 17..22)
    DataId   sensorId;   // capteur META de type Sensor et de nature metrique
    uint8_t  cmp;        // CMP_LOWER ou CMP_GREATER_OR_EQUAL
    float    threshold;  // borné par les min/max META du capteur
    uint8_t  startHour;  // 0..23 (heure locale)
    uint8_t  endHour;    // 0..23 (heure locale), dernière heure incluse
    uint8_t  restHours;  // 1..24, repos après un déclenchement
    uint16_t duration;   // secondes, borné par les min/max META de cmdId
};

// ═════════════════════════════════════════════════════════════════════════════
// ConditionalWatering — API statique
// ═════════════════════════════════════════════════════════════════════════════

class ConditionalWatering {
public:
    // Topics MQTT (publics pour MqttManager)
    static constexpr const char* CONDITIONAL_TOPIC_FROM_USER = "serre/conditional/FromUser";
    static constexpr const char* CONDITIONAL_TOPIC_TO_USER   = "serre/conditional/ToUser";

    // Comparateurs. Exhaustifs et sans recouvrement : une valeur tombe
    // toujours dans exactement l'un des deux cas.
    static constexpr uint8_t CMP_LOWER            = 0;  // valeur <  seuil
    static constexpr uint8_t CMP_GREATER_OR_EQUAL = 1;  // valeur >= seuil

    // Charge /conditional.json. Appeler une fois au boot, après LittleFS.begin().
    static void init();

    // Tâche périodique. Traite les messages MQTT bufferisés, puis évalue les
    // règles concernées par les mesures reçues depuis le tick précédent.
    static void handle();

    // Réception d'une donnée du bus. Appelée par DataBus::distribute() depuis
    // le thread du producteur — se contente de mémoriser les mesures utiles.
    static void onNewData(const BusItem& item);

    // Réception d'un message MQTT sur serre/conditional/FromUser.
    // Appelée depuis le thread esp_mqtt — bufferise le message brut pour
    // traitement dans handle() (thread TaskManager).
    static void onConditionalMessage(const char* data, int len);

    // Sérialise l'état courant et publie via MqttManager (retain).
    static void publishConditionalState();

private:
    static constexpr uint8_t MAX_CONDITIONAL_RULES = 16;
    static constexpr uint8_t MIN_REST_HOURS        = 1;
    static constexpr uint8_t MAX_REST_HOURS        = 24;

    static ConditionalRule conditionalRules[];
    static uint8_t         conditionalRuleCount;

    // Instant du dernier déclenchement de chaque règle (timestamp UTC),
    // 0 = jamais déclenchée. Tableau parallèle à conditionalRules[] : toute
    // suppression doit déplacer les deux entrées de la même façon.
    // RAM seule, jamais persisté.
    static uint32_t conditionalRuleLastTrigger[];

    // ─── Mesures en attente d'évaluation ─────────────────────────────────
    // Écrites par onNewData() (thread producteur), lues et vidées par
    // handle() (thread TaskManager). Protégées par portMUX, comme
    // lastDataForWeb. Une seule entrée par capteur : une mesure plus récente
    // écrase la précédente si le tick n'est pas encore passé.
    struct PendingMeasure {
        DataId sensorId;
        float  value;
    };

    static constexpr uint8_t MAX_PENDING_MEASURES = 8;

    static PendingMeasure pendingMeasures[];
    static uint8_t        pendingMeasureCount;
    static portMUX_TYPE   pendingMeasureMux;

    // Buffer MQTT (thread esp_mqtt → thread TaskManager).
    // Un seul writer (onConditionalMessage), un seul reader (handle).
    // Si un nouveau message arrive avant traitement, il écrase le précédent.
    static constexpr size_t MSG_BUFFER_SIZE = 384;
    static char          conditionalMsgBuffer[];
    static volatile bool conditionalMsgPending;

    // Traitement du message bufferisé (appelé depuis handle).
    static void processConditionalMessage();

    // Évaluation des règles utilisant ce capteur, avec la valeur reçue.
    static void evaluateRulesForSensor(DataId sensorId, float value,
                                       uint32_t nowTs, uint8_t localHour);

    // Ajout/suppression avec validation complète + sauvegarde.
    static bool addConditionalRule(const ConditionalRule& rule);
    static bool removeConditionalRule(const ConditionalRule& rule);

    // Validation des champs d'une règle (bornes META, cmdId routé par RELAYS[]).
    static bool validateConditionalRule(const ConditionalRule& rule);

    // Égalité de deux règles sur la totalité de leurs champs (identité).
    static bool sameConditionalRule(const ConditionalRule& a,
                                    const ConditionalRule& b);

    // Persistance LittleFS (écriture atomique via .tmp + rename).
    static bool saveConditionalRules();
    static bool loadConditionalRules();

    // Sérialisation JSON (format commun persistance + MQTT ToUser).
    static String serializeConditionalRules();
};
