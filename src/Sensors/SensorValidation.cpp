// Sensors/SensorValidation.cpp
// Validation de fiabilité des capteurs — voir SensorValidation.h

#include "Sensors/SensorValidation.h"
#include "Sensors/SoilSensorRS485.h"
#include "Sensors/AirSensorRS485.h"
#include "Sensors/InboxSensorRS485.h"
#include "Gardener/ConditionalWatering.h"
#include "Core/DataBus.h"
#include "Core/VirtualClock.h"
#include "Utils/Console.h"

#include <math.h>
#include <string.h>

// ─── Variables statiques ─────────────────────────────────────────────────────

SensorValidation::SensorSlot SensorValidation::slots[SLOT_MAX] = {};
uint8_t SensorValidation::slotCount = 0;

// ─── collect() ───────────────────────────────────────────────────────────────
// Recopie les DataId déclarés par un module capteur, en ne retenant que ceux
// de type Sensor et de nature metrique dans META.

void SensorValidation::collect(uint8_t count, DataId (*at)(uint8_t),
                               uint8_t (*addrOf)(DataId))
{
    for (uint8_t i = 0; i < count; i++) {
        if (slotCount >= SLOT_MAX) {
            Console::warn(TAG, "Plus de capteurs que SLOT_MAX ("
                              + String(SLOT_MAX) + ") — surplus ignoré");
            return;
        }

        DataId id = at(i);
        const DataMeta& meta = getMeta(id);

        if (meta.type != DataType::Sensor || meta.nature != DataNature::metrique)
            continue;

        SensorSlot& s   = slots[slotCount];
        s.id            = id;
        s.rs485Address  = addrOf(id);
        s.windowCount   = 0;
        s.windowIndex   = 0;
        s.spikeThreshold = (strcmp(meta.unit, "%") == 0)
                           ? SPIKE_THRESHOLD_HUM
                           : SPIKE_THRESHOLD_TEMP;
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

    collect(SoilSensorRS485::measurableCount(),
            &SoilSensorRS485::measurableAt,
            &SoilSensorRS485::rs485AddressOf);

    collect(AirSensorRS485::measurableCount(),
            &AirSensorRS485::measurableAt,
            &AirSensorRS485::rs485AddressOf);

    collect(InboxSensorRS485::measurableCount(),
            &InboxSensorRS485::measurableAt,
            &InboxSensorRS485::rs485AddressOf);

    Console::info(TAG, String(slotCount)
                  + " capteur(s) métrique(s) sous validation de fiabilité");

    publishSynthetic();
}

// ─── feed() ──────────────────────────────────────────────────────────────────
// Point d'entrée appelé par les modules capteurs après chaque lecture réussie.
//
// Séquence :
//   1. Fenêtre glissante → test spike
//   2. Valeur mémorisée + horodatage → test stuck
//   3. Si fiable → ConditionalWatering::offerMeasure()
//   4. Retourne true si l'état de validation a changé

bool SensorValidation::feed(DataId sensorId, float value)
{
    SensorSlot* s = nullptr;
    if (!findSlot(sensorId, s)) return false;

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
// grandeur physique (°C / %), identifié par adresse RS485.
// Format : °C OK:1-2 Figé:3 Dysf:4 Abs:13 | % OK:1-2-3 Figé: Dysf: Abs:
// Caractères CSV-safe : pas de virgule ni guillemet.

void SensorValidation::publishSynthetic()
{
    char buf[200];
    size_t pos = 0;

    for (uint8_t grp = 0; grp < 2; grp++) {
        bool wantHygro = (grp == 1);

        if (grp == 0) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "\xC2\xB0""C");
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " | %%");
        }

        const char* stateLabels[] = { " OK:", " Fig\xC3\xA9:", " Dysf:", " Abs:" };

        for (uint8_t st = 0; st < 4; st++) {
            if (pos >= sizeof(buf) - 1) break;
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", stateLabels[st]);

            bool first = true;
            for (uint8_t i = 0; i < slotCount && pos < sizeof(buf) - 1; i++) {
                const SensorSlot& s = slots[i];
                const DataMeta& meta = getMeta(s.id);

                bool isTemp  = (strcmp(meta.unit, "\xC2\xB0""C") == 0);
                bool isHygro = (strcmp(meta.unit, "%") == 0);
                if (!isTemp && !isHygro) continue;
                if (wantHygro != isHygro) continue;

                uint8_t slotState;
                if (s.absentAlert)     slotState = 3;
                else if (s.stuckAlert) slotState = 1;
                else if (s.spikeAlert) slotState = 2;
                else                   slotState = 0;

                if (slotState != st) continue;

                if (!first && pos < sizeof(buf) - 1) buf[pos++] = '-';
                first = false;
                pos += snprintf(buf + pos, sizeof(buf) - pos, "%u", s.rs485Address);
            }
        }
    }

    if (pos >= sizeof(buf)) pos = sizeof(buf) - 1;
    buf[pos] = '\0';

    Console::info(TAG, buf);

    BusItem item = {};
    item.type      = DataType::System;
    item.id        = DataId::SensorHealth;
    item.valueKind = 1;
    memcpy(item.valueText, buf, pos + 1);
    DataBus::publish(item);
}
