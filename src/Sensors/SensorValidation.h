// Sensors/SensorValidation.h
//
// Suivi de présence de toutes les mesures, et validation de fiabilité de la
// température et de l'hygrométrie avant arrosage conditionnel.
//
// Deux mécanismes indépendants, par capteur :
//
//   1. Détection de sauts (spike) — fenêtre glissante de 12 valeurs (≈ 1 h à
//      5 min par lecture). Si l'écart max − min dans la fenêtre dépasse le
//      seuil (50 °C ou 80 %), le capteur est jugé instable.
//
//   2. Détection de valeur figée (stuck) — si la valeur mesurée ne varie pas
//      de plus de VALUE_EPSILON pendant STUCK_TIMEOUT_S (6 h), le capteur est
//      jugé bloqué.
//
// Tant que la fenêtre n'est pas pleine (démarrage), le capteur est considéré
// fiable. Dès que le comportement redevient normal, le capteur est
// immédiatement réhabilité (auto-guérison sans délai).
//
// Ce module ne cadence rien : c'est le module capteur qui pousse chaque mesure
// via feed(). Si la mesure est fiable, elle est transmise à
// ConditionalWatering::offerMeasure(). Sinon, la mesure n'est PAS transmise.
// Quand un capteur ne répond pas, le module capteur appelle feedNoResponse().
//
// À chaque changement d'état de validation (passage OK↔Figé↔Dysf↔Abs),
// un message synthétique unique est publié sur DataBus (DataId::SensorHealth, texte)
// donnant l'état de TOUS les capteurs en une ligne compacte, groupé par
// grandeur NEO et identifié par adresse RS485. L'en-tête de chaque groupe est
// le symbole de la grandeur (grandeurSymbol, Config/Neo.h).
// Format : °C OK:1-2-3 Figé:4 Dysf: Abs: | % OK:1-2 Figé: Dysf: Abs: | Pile OK:9 Figé: Dysf: Abs:10
//
// Deux rôles distincts, dont un seul est de portée restreinte :
//
//   Suivi de présence — toute mesure métrique déclarée dans NEO a son entrée
//   dans la table. Recevoir une valeur dit que la sonde répond, ne rien
//   recevoir dit qu'elle est absente. Ce suivi ne suppose rien de la nature
//   de la mesure : il vaut pour la vingt-cinquième grandeur comme pour la
//   première, sans qu'une ligne de ce module ait à la nommer.
//
//   Jugement de vraisemblance — les deux mécanismes ci-dessus ne s'appliquent
//   qu'à la température et à l'hygrométrie, seules grandeurs dont une valeur
//   fausse peut déclencher un arrosage à tort. Elles sont nommées à un seul
//   endroit, dans buildSlotsFromNeo(), avec le seuil qui les décrit. Le reste
//   est suivi sans être jugé : ni saut, ni valeur figée, et pas d'offre à
//   ConditionalWatering. La charge de pile d'une sonde sans fil, qui ne bouge
//   que de quelques pour cent par mois, apparaît donc en OK ou en Abs et
//   jamais en Figé.
//
// isJudged() expose cette distinction au dehors : ConditionalWatering s'en
// sert pour refuser une règle écrite sur une grandeur dont la mesure ne lui
// parviendra jamais.
//
// La grandeur et l'adresse RS485 sont lues dans l'entrée NEO. Elles ne sont
// plus déduites du texte de META.unit, qui vaut « % » aussi bien pour une
// hygrométrie que pour une charge de pile.
//
// État en RAM seule, perdu au reboot (choix cohérent avec
// ConditionalWatering::conditionalRuleLastTrigger).
//
// Intégration :
//   - init() appelé dans loopInit() après Neo::build() et avant
//     ConditionalWatering::init() ; publie un digest initial
//     « tout OK » sur DataId::SensorHealth (retain MQTT)
//   - feed() appelé par SoilSensorRS485, AirSensorRS485, InboxSensorRS485
//     à la place de ConditionalWatering::offerMeasure()
#pragma once

#include <Arduino.h>
#include "Config/MetaDataModel.h"
#include "Config/Neo.h"

class SensorValidation {
public:
    // Construit la table id → état par parcours de NEO.
    // À appeler après Neo::build(). Publie ensuite le digest initial sur
    // SensorHealth.
    static void init();

    // Point d'entrée unique. Appelé par chaque module capteur après lecture
    // matérielle, à la place de ConditionalWatering::offerMeasure().
    // Si la mesure est fiable, transmet à offerMeasure(). Sinon, ne transmet
    // pas. Retourne true si l'état de validation a changé.
    static bool feed(DataId sensorId, float value);

    // Signale qu'un capteur n'a pas répondu (timeout Modbus).
    // Appelé par les modules capteurs quand readOne()/readHardware() échoue.
    // Retourne true si l'état de validation a changé.
    static bool feedNoResponse(DataId sensorId);

    // Vrai si ce module juge la vraisemblance des mesures de cet id, donc si
    // elles peuvent atteindre ConditionalWatering::offerMeasure(). Faux pour
    // une grandeur seulement suivie en présence, et pour un id absent de la
    // table. Une règle conditionnelle écrite sur un id qui ne le satisfait pas
    // ne se déclencherait jamais.
    static bool isJudged(DataId sensorId);

    // Construit et publie le message synthétique global sur DataId::SensorHealth.
    // À appeler par le module capteur après avoir traité TOUTES les grandeurs
    // d'un capteur physique, si au moins un appel feed/feedNoResponse a
    // retourné true.
    static void publishSynthetic();

private:
    static constexpr const char* TAG = "SensorValid";

    static constexpr uint8_t  SLOT_MAX            = (uint8_t)Neo::MAX;
    static constexpr uint8_t  WINDOW_SIZE         = 12;
    static constexpr float    SPIKE_THRESHOLD_TEMP = 50.0f;   // °C
    static constexpr float    SPIKE_THRESHOLD_HUM  = 80.0f;   // %
    static constexpr uint32_t STUCK_TIMEOUT_S      = 6 * 3600;
    static constexpr float    VALUE_EPSILON        = 0.05f;

    struct SensorSlot {
        DataId   id;
        uint8_t  rs485Address;
        Grandeur grandeur;

        // Faux : mesure suivie en présence seulement. Les deux mécanismes
        // ci-dessous sont alors inactifs et leurs drapeaux restent à false.
        bool     judged;

        // Fenêtre glissante (spike)
        float    window[WINDOW_SIZE];
        uint8_t  windowCount;
        uint8_t  windowIndex;
        float    spikeThreshold;

        // Détection valeur figée (stuck)
        float    lastValue;
        uint32_t lastChangeTs;
        bool     initialized;

        // Drapeaux d'alerte (état courant)
        bool     spikeAlert;
        bool     stuckAlert;
        bool     absentAlert;
    };

    static SensorSlot slots[SLOT_MAX];
    static uint8_t    slotCount;

    // Parcourt NEO et retient les entrées de type Sensor et de nature
    // metrique dans META.
    static void buildSlotsFromNeo();

    // Recherche linéaire dans slots[].
    static bool findSlot(DataId id, SensorSlot*& out);
};
