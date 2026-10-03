// Sensors/OnDemandMeasure.cpp
// Mesure à la demande — voir OnDemandMeasure.h pour l'architecture.
//
// Chaîne complète d'une demande :
//   Interface → serre/ondemand/FromUser → MqttManager (thread esp_mqtt)
//     → OnDemandMeasure::onRequest : valide, pose l'id dans le slot, rend la main
//     → OnDemandMeasure::handle    : (thread TaskManager) appelle le pointeur
//                                    de mesure porté par l'entrée NEO de l'id
//     → SoilSensorRS485 | AirSensorRS485 | InboxSensorRS485 | SupplyVoltage :: measureNow
//     → DataBus::publish           : chemin normal (CSV, MQTT, page web)
//     → serre/data/{id}            : l'interface se met à jour d'elle-même

#include "Sensors/OnDemandMeasure.h"
#include "Config/Neo.h"
#include "Utils/Console.h"

#include <ArduinoJson.h>
#include <string.h>

// ─── Variables statiques ─────────────────────────────────────────────────────

volatile bool    OnDemandMeasure::requestPending = false;
volatile uint8_t OnDemandMeasure::requestedId    = 0;

// ─────────────────────────────────────────────────────────────────────────────
// init
//
// Rien à construire : NEO porte déjà, pour chaque donnée installée, le
// pointeur du module qui sait la mesurer. Le décompte affiché n'est là que
// pour la trace de démarrage.
// ─────────────────────────────────────────────────────────────────────────────

void OnDemandMeasure::init()
{
    requestPending = false;
    requestedId    = 0;

    uint8_t measurable = 0;
    for (uint8_t i = 0; i < Neo::count(); i++) {
        if (Neo::at(i).measure != nullptr) measurable++;
    }

    Console::info(TAG, String(measurable) + " donnée(s) mesurable(s) à la demande "
                       "— déclarées dans NEO par les modules producteurs");
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

    const NeoEntry* entry = Neo::find((DataId)idVal);
    if (entry == nullptr || entry->measure == nullptr) {
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

    const NeoEntry* entry = Neo::find(id);
    if (entry == nullptr || entry->measure == nullptr) return;   // déjà validé à la réception

    if (entry->measure(id)) {
        Console::info(TAG, "Mesure à la demande publiée — id="
                          + String((uint8_t)id)
                          + " (" + String(getMeta(id).label) + ")");
    } else {
        Console::warn(TAG, "Mesure à la demande sans résultat — id="
                          + String((uint8_t)id)
                          + " (" + String(getMeta(id).label) + ")");
    }
}
