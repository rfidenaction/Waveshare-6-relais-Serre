// Sensors/SensorValidation.h
//
// Validation de fiabilité des capteurs métriques avant arrosage conditionnel.
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
// grandeur physique (Temp / Hygro) et identifié par adresse RS485.
// Format : °C OK:1-2-3 Figé:4 Dysf: Abs: | % OK:1-2-3-4 Figé: Dysf: Abs:
//
// La vue id → état est construite à l'init par interrogation des modules
// capteurs (même pattern que OnDemandMeasure). Seuls les DataId de type
// Sensor et de nature metrique sont retenus — SupplyVoltage (type Power) est
// donc naturellement exclu.
//
// État en RAM seule, perdu au reboot (choix cohérent avec
// ConditionalWatering::conditionalRuleLastTrigger).
//
// Intégration :
//   - init() appelé dans loopInit() après les init() des 3 modules capteurs
//     et avant ConditionalWatering::init() ; publie un digest initial
//     « tout OK » sur DataId::SensorHealth (retain MQTT)
//   - feed() appelé par SoilSensorRS485, AirSensorRS485, InboxSensorRS485
//     à la place de ConditionalWatering::offerMeasure()
#pragma once

#include <Arduino.h>
#include "Config/MetaDataModel.h"

class SensorValidation {
public:
    // Construit la table id → état en interrogeant les modules capteurs.
    // À appeler après les init() de SoilSensorRS485, AirSensorRS485,
    // InboxSensorRS485. Publie ensuite le digest initial sur SensorHealth.
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

    // Construit et publie le message synthétique global sur DataId::SensorHealth.
    // À appeler par le module capteur après avoir traité TOUTES les grandeurs
    // d'un capteur physique, si au moins un appel feed/feedNoResponse a
    // retourné true.
    static void publishSynthetic();

private:
    static constexpr const char* TAG = "SensorValid";

    static constexpr uint8_t  SLOT_MAX            = 32;
    static constexpr uint8_t  WINDOW_SIZE         = 12;
    static constexpr float    SPIKE_THRESHOLD_TEMP = 50.0f;   // °C
    static constexpr float    SPIKE_THRESHOLD_HUM  = 80.0f;   // %
    static constexpr uint32_t STUCK_TIMEOUT_S      = 6 * 3600;
    static constexpr float    VALUE_EPSILON        = 0.05f;

    struct SensorSlot {
        DataId   id;
        uint8_t  rs485Address;

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

    // Agrège les DataId déclarés par un module capteur. Ne retient que ceux
    // de type Sensor et de nature metrique dans META.
    static void collect(uint8_t count, DataId (*at)(uint8_t),
                        uint8_t (*addrOf)(DataId));

    // Recherche linéaire dans slots[].
    static bool findSlot(DataId id, SensorSlot*& out);
};
