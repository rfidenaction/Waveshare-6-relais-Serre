// Sensors/SensorValidation.cpp
// Validation de fiabilité des capteurs — voir SensorValidation.h

#include "Sensors/SensorValidation.h"
#include "Config/Neo.h"
#include "Gardener/ConditionalWatering.h"
#include "Core/DataBus.h"
#include "Core/VirtualClock.h"
#include "Utils/Console.h"

#include <math.h>
#include <string.h>

// ─── Variables statiques ─────────────────────────────────────────────────────

SensorValidation::SensorSlot SensorValidation::slots[SLOT_MAX] = {};
uint8_t SensorValidation::slotCount = 0;

// ─── buildSlotsFromNeo() ─────────────────────────────────────────────────────
// Parcourt NEO et donne une entrée à toute mesure métrique, quelle que soit sa
// grandeur : le suivi de présence les concerne toutes. Seules la température
// et l'hygrométrie sont en plus jugées, et ce sont les deux seuls noms de
// grandeur que ce module prononce. La grandeur et l'adresse RS485 sont lues
// dans l'entrée NEO, plus déduites du libellé ni demandées au module.

void SensorValidation::buildSlotsFromNeo()
{
    for (uint8_t i = 0; i < Neo::count(); i++) {
        if (slotCount >= SLOT_MAX) {
            Console::warn(TAG, "Plus de capteurs que SLOT_MAX ("
                              + String(SLOT_MAX) + ") — surplus ignoré");
            return;
        }

        const NeoEntry& entry = Neo::at(i);
        const DataMeta& meta  = getMeta(entry.id);

        if (meta.type != DataType::Sensor || meta.nature != DataNature::metrique)
            continue;

        // Être jugé et avoir un seuil de saut sont une seule et même décision :
        // une grandeur est jugée parce que ce module sait la juger, et le seuil
        // est celui qui la décrit. Toute autre mesure garde judged à false et
        // n'hérite du seuil d'aucune autre — elle est suivie en présence, sans
        // qu'il soit besoin de savoir de quelle grandeur il s'agit.
        bool  judged         = false;
        float spikeThreshold = 0.0f;

        if (entry.grandeur == Grandeur::Temperature) {
            judged         = true;
            spikeThreshold = SPIKE_THRESHOLD_TEMP;
        } else if (entry.grandeur == Grandeur::Humidite) {
            judged         = true;
            spikeThreshold = SPIKE_THRESHOLD_HUM;
        }

        SensorSlot& s   = slots[slotCount];
        s.id            = entry.id;
        s.rs485Address  = entry.rs485Address;
        s.grandeur      = entry.grandeur;
        s.judged        = judged;
        s.windowCount   = 0;
        s.windowIndex   = 0;
        s.spikeThreshold = spikeThreshold;
        s.lastValue     = 0.0f;
        s.lastChangeTs  = 0;
        s.initialized   = false;
        s.spikeAlert    = false;
        s.stuckAlert    = false;
        s.absentAlert   = false;

        slotCount++;
    }
}

// ─── findSlot() ──────────────────────────────────────────────────────────────

bool SensorValidation::findSlot(DataId id, SensorSlot*& out)
{
    for (uint8_t i = 0; i < slotCount; i++) {
        if (slots[i].id == id) {
            out = &slots[i];
            return true;
        }
    }
    return false;
}

// ─── init() ──────────────────────────────────────────────────────────────────

void SensorValidation::init()
{
    slotCount = 0;

    buildSlotsFromNeo();

    Console::info(TAG, String(slotCount)
                  + " capteur(s) métrique(s) sous validation de fiabilité");

    publishSynthetic();
}

// ─── feed() ──────────────────────────────────────────────────────────────────
// Point d'entrée appelé par les modules capteurs après chaque lecture réussie.
//
// Séquence :
//   0. Mesure non jugée → présence seule, on s'arrête là
//   1. Fenêtre glissante → test spike
//   2. Valeur mémorisée + horodatage → test stuck
//   3. Si fiable → ConditionalWatering::offerMeasure()
//   4. Retourne true si l'état de validation a changé

bool SensorValidation::feed(DataId sensorId, float value)
{
    SensorSlot* s = nullptr;
    if (!findSlot(sensorId, s)) return false;

    // Mesure suivie mais non jugée : recevoir une valeur dit que la sonde
    // répond, et rien de plus. Ni test de vraisemblance — les seuils de saut
    // et le délai de valeur figée ne décrivent pas cette grandeur — ni offre
    // à l'arrosage conditionnel.
    if (!s->judged) {
        bool wasAbsent = s->absentAlert;
        s->absentAlert = false;
        return (s->absentAlert != wasAbsent);
    }

    bool wasSpike  = s->spikeAlert;
    bool wasStuck  = s->stuckAlert;
    bool wasAbsent = s->absentAlert;

    // Réception d'une valeur → le capteur répond
    s->absentAlert = false;

    // ── 1. Fenêtre glissante (spike) ─────────────────────────────────────

    s->window[s->windowIndex] = value;
    s->windowIndex = (s->windowIndex + 1) % WINDOW_SIZE;
    if (s->windowCount < WINDOW_SIZE) s->windowCount++;

    if (s->windowCount == WINDOW_SIZE) {
        float wMin = s->window[0];
        float wMax = s->window[0];
        for (uint8_t i = 1; i < WINDOW_SIZE; i++) {
            if (s->window[i] < wMin) wMin = s->window[i];
            if (s->window[i] > wMax) wMax = s->window[i];
        }
        s->spikeAlert = ((wMax - wMin) > s->spikeThreshold);
    } else {
        s->spikeAlert = false;
    }

    // ── 2. Détection valeur figée (stuck) ────────────────────────────────

    TimeVClock t = VirtualClock::read();

    if (t.VClock_available) {
        uint32_t nowTs = (uint32_t)t.timestamp;

        if (!s->initialized) {
            s->lastValue    = value;
            s->lastChangeTs = nowTs;
            s->initialized  = true;
            s->stuckAlert   = false;
        } else if (fabsf(value - s->lastValue) >= VALUE_EPSILON) {
            s->lastValue    = value;
            s->lastChangeTs = nowTs;
            s->stuckAlert   = false;
        } else {
            if (nowTs >= s->lastChangeTs &&
                (nowTs - s->lastChangeTs) >= STUCK_TIMEOUT_S) {
                s->stuckAlert = true;
            }
        }
    }

    // ── 3. Décision ──────────────────────────────────────────────────────

    if (!s->spikeAlert && !s->stuckAlert) {
        ConditionalWatering::offerMeasure(sensorId, value);
    }

    // ── 4. Signale si l'état a changé (l'appelant décide de publier) ─────

    return (s->spikeAlert != wasSpike) ||
           (s->stuckAlert != wasStuck) ||
           (s->absentAlert != wasAbsent);
}

// ─── isJudged() ──────────────────────────────────────────────────────────────
// Dit si les mesures de cet id sont soumises au jugement de vraisemblance, donc
// si elles peuvent atteindre ConditionalWatering::offerMeasure().

bool SensorValidation::isJudged(DataId sensorId)
{
    SensorSlot* s = nullptr;
    if (!findSlot(sensorId, s)) return false;

    return s->judged;
}

// ─── feedNoResponse() ────────────────────────────────────────────────────────
// Signale qu'un capteur n'a pas répondu (timeout Modbus). Positionne le
// drapeau absentAlert. Retourne true si c'est un changement d'état.

bool SensorValidation::feedNoResponse(DataId sensorId)
{
    SensorSlot* s = nullptr;
    if (!findSlot(sensorId, s)) return false;

    if (!s->absentAlert) {
        s->absentAlert = true;
        return true;
    }
    return false;
}

// ─── publishSynthetic() ──────────────────────────────────────────────────────
// Construit un message compact donnant l'état de tous les capteurs, groupé par
// grandeur NEO et identifié par adresse RS485. L'en-tête de chaque groupe est
// le symbole de la grandeur (grandeurSymbol, Config/Neo.h).
// Format : °C OK:1-2 Figé:3 Dysf:4 Abs:13 | % OK:1-2-3 Figé: Dysf: Abs: | Pile OK:9 Figé: Dysf: Abs:10
// Caractères CSV-safe : pas de virgule ni guillemet.
//
// La boucle parcourt les valeurs de Grandeur et n'émet un groupe que si au
// moins un capteur le porte. Aucune liste de grandeurs n'est tenue ici : une
// grandeur nouvelle apparaît dans le message du seul fait d'être déclarée
// dans NEO, et un capteur n'entre dans un groupe que sur égalité exacte.
//
// Le message est assemblé dans une String puis recopié en une fois dans le
// BusItem, comme le font StatusReport, SmsManager et BridgeManager. C'est le
// strncpy final qui coupe si la ligne dépasse, une bonne fois, plutôt qu'une
// arithmétique d'offset à surveiller à chaque écriture.

void SensorValidation::publishSynthetic()
{
    static const char* const stateLabels[] = { " OK:", " Fig\xC3\xA9:", " Dysf:", " Abs:" };

    String msg;
    msg.reserve(sizeof(BusItem::valueText));

    for (uint8_t g = 0; g <= (uint8_t)Grandeur::Debit; g++) {
        const Grandeur grandeur = (Grandeur)g;

        bool present = false;
        for (uint8_t i = 0; i < slotCount; i++) {
            if (slots[i].grandeur == grandeur) { present = true; break; }
        }
        if (!present) continue;

        if (msg.length() > 0) msg += " | ";
        msg += grandeurSymbol(grandeur);

        for (uint8_t st = 0; st < 4; st++) {
            msg += stateLabels[st];

            bool first = true;
            for (uint8_t i = 0; i < slotCount; i++) {
                const SensorSlot& s = slots[i];

                if (s.grandeur != grandeur) continue;

                uint8_t slotState;
                if (s.absentAlert)     slotState = 3;
                else if (s.stuckAlert) slotState = 1;
                else if (s.spikeAlert) slotState = 2;
                else                   slotState = 0;

                if (slotState != st) continue;

                if (!first) msg += '-';
                first = false;
                msg += String((unsigned)s.rs485Address);
            }
        }
    }

    Console::info(TAG, msg);

    BusItem item = {};
    item.type      = DataType::System;
    item.id        = DataId::SensorHealth;
    item.valueKind = 1;
    strncpy(item.valueText, msg.c_str(), sizeof(item.valueText) - 1);
    item.valueText[sizeof(item.valueText) - 1] = '\0';
    DataBus::publish(item);
}
