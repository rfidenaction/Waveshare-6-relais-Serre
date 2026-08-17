// Sensors/OnDemandMeasure.cpp
// Mesure à la demande — voir OnDemandMeasure.h pour l'architecture.
//
// Chaîne complète d'une demande :
//   Interface → serre/ondemand/FromUser → MqttManager (thread esp_mqtt)
//     → OnDemandMeasure::onRequest : valide, pose l'id dans le slot, rend la main
//     → OnDemandMeasure::handle    : (thread TaskManager) dispatch vers le module
//     → SoilSensorRS485 | AirSensorRS485 | SupplyVoltage :: measureNow
//     → DataBus::publish           : chemin normal (CSV, MQTT, page web)
//     → serre/data/{id}            : l'interface se met à jour d'elle-même

#include "Sensors/OnDemandMeasure.h"
#include "Sensors/SupplyVoltage.h"
#include "Sensors/SoilSensorRS485.h"
#include "Sensors/AirSensorRS485.h"
#include "Utils/Console.h"

#include <ArduinoJson.h>
#include <string.h>

// ─── Variables statiques ─────────────────────────────────────────────────────

OnDemandMeasure::MeasurableSlot OnDemandMeasure::slots[MEASURABLE_MAX] = {};
uint8_t OnDemandMeasure::slotCount = 0;

volatile bool    OnDemandMeasure::requestPending = false;
volatile uint8_t OnDemandMeasure::requestedId    = 0;

// ─────────────────────────────────────────────────────────────────────────────
// collect — recopie la liste déclarée par un module producteur
// ─────────────────────────────────────────────────────────────────────────────

void OnDemandMeasure::collect(uint8_t count, DataId (*at)(uint8_t), MeasureFn measure)
{
    for (uint8_t i = 0; i < count; i++) {
        if (slotCount >= MEASURABLE_MAX) {
            Console::warn(TAG, "Plus de données mesurables que MEASURABLE_MAX ("
                              + String(MEASURABLE_MAX) + ") — surplus ignoré");
            return;
        }
        slots[slotCount].id      = at(i);
        slots[slotCount].measure = measure;
        slotCount++;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// buildSlotsFromSensors — construction de la vue id → propriétaire
//
// Les modules déclarent, OnDemandMeasure agrège. Aucune correspondance n'est
// écrite ici : ajouter un capteur dans le SENSORS[] de son module suffit.
// ─────────────────────────────────────────────────────────────────────────────

void OnDemandMeasure::buildSlotsFromSensors()
{
    slotCount = 0;

    collect(SupplyVoltage::measurableCount(),
            &SupplyVoltage::measurableAt,
            &SupplyVoltage::measureNow);

    collect(SoilSensorRS485::measurableCount(),
            &SoilSensorRS485::measurableAt,
            &SoilSensorRS485::measureNow);

    collect(AirSensorRS485::measurableCount(),
            &AirSensorRS485::measurableAt,
            &AirSensorRS485::measureNow);
}

// ─────────────────────────────────────────────────────────────────────────────
// findSlot — recherche linéaire dans la vue
// ─────────────────────────────────────────────────────────────────────────────

bool OnDemandMeasure::findSlot(DataId id, MeasurableSlot*& outSlot)
{
    for (uint8_t i = 0; i < slotCount; i++) {
        if (slots[i].id == id) {
            outSlot = &slots[i];
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// init
// ─────────────────────────────────────────────────────────────────────────────

void OnDemandMeasure::init()
{
    buildSlotsFromSensors();

    requestPending = false;
    requestedId    = 0;

    Console::info(TAG, String(slotCount) + " donnée(s) mesurable(s) à la demande "
                       "— déclarées par les modules producteurs");
}

// ─────────────────────────────────────────────────────────────────────────────
// onRequest — thread esp_mqtt. Valide et pose dans le slot. Jamais d'I/O bus.
// ─────────────────────────────────────────────────────────────────────────────

void OnDemandMeasure::onRequest(const char* data, int len)
{
    if (!data || len <= 0 || len > 64) {
        Console::warn(TAG, "Demande rejetée — taille de payload invalide");
        return;
    }

    char buf[65];
    memcpy(buf, data, len);
    buf[len] = '\0';

    StaticJsonDocument<96> doc;
    DeserializationError err = deserializeJson(doc, buf);
    if (err) {
        Console::warn(TAG, "Demande rejetée — JSON malformé : " + String(err.c_str()));
        return;
    }

    const char* op = doc["op"] | "";
    if (strcmp(op, "measure") != 0) {
        Console::warn(TAG, "Demande rejetée — op inconnu : " + String(op));
        return;
    }

    int idVal = doc["id"] | -1;
    if (idVal < 0 || idVal > 255 || !isValidId((uint8_t)idVal)) {
        Console::warn(TAG, "Demande rejetée — id inconnu de META : " + String(idVal));
        return;
    }

    MeasurableSlot* slot = nullptr;
    if (!findSlot((DataId)idVal, slot)) {
        Console::warn(TAG, "Demande rejetée — id=" + String(idVal)
                          + " n'est mesurable par aucun module");
        return;
    }

    if (requestPending) {
        Console::warn(TAG, "Demande ignorée — une mesure est déjà en attente");
        return;
    }

    // Ordre significatif : l'id avant le drapeau (voir OnDemandMeasure.h).
    requestedId    = (uint8_t)idVal;
    requestPending = true;

    Console::info(TAG, "Demande de mesure acceptée — id=" + String(idVal)
                      + " (" + String(getMeta((DataId)idVal).label) + ")");
}

// ─────────────────────────────────────────────────────────────────────────────
// handle — thread TaskManager. Exécute la mesure en attente.
//
// S'exécutant dans le même thread que les handle() périodiques des capteurs,
// il ne peut jamais entrer en concurrence avec eux sur Serial1.
// ─────────────────────────────────────────────────────────────────────────────

void OnDemandMeasure::handle()
{
    if (!requestPending) return;

    DataId id = (DataId)requestedId;
    requestPending = false;

    MeasurableSlot* slot = nullptr;
    if (!findSlot(id, slot)) return;    // déjà validé à la réception

    if (slot->measure(id)) {
        Console::info(TAG, "Mesure à la demande publiée — id="
                          + String((uint8_t)id)
                          + " (" + String(getMeta(id).label) + ")");
    } else {
        Console::warn(TAG, "Mesure à la demande sans résultat — id="
                          + String((uint8_t)id)
                          + " (" + String(getMeta(id).label) + ")");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Énumération pour le schéma MQTT
// ─────────────────────────────────────────────────────────────────────────────

uint8_t OnDemandMeasure::measurableCount()
{
    return slotCount;
}

DataId OnDemandMeasure::measurableAt(uint8_t index)
{
    if (index >= slotCount) index = 0;   // garde : index hors bornes
    return slots[index].id;
}
