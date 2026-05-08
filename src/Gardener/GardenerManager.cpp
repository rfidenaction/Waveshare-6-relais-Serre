// src/Gardener/GardenerManager.cpp
//
// Programmateur d'arrosage automatique — voir GardenerManager.h

#include "Gardener/GardenerManager.h"
#include "Connectivity/MqttManager.h"
#include "Core/DataBus.h"
#include "Core/VirtualClock.h"
#include "Config/IO-Config.h"
#include "Utils/Console.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>

static const char* TAG = "Gardener";

// ─── Variables statiques ─────────────────────────────────────────────────────

GardenerWateringSlot GardenerManager::gardenerWateringSlots[MAX_WATERING_SLOTS_TOTAL];
uint8_t  GardenerManager::gardenerWateringSlotCount = 0;
uint16_t GardenerManager::gardenerLastMinute        = 0xFFFF;

char          GardenerManager::gardenerMsgBuffer[MSG_BUFFER_SIZE] = {};
volatile bool GardenerManager::gardenerMsgPending = false;

// ─── init() ──────────────────────────────────────────────────────────────────

void GardenerManager::init()
{
    loadGardenerWateringSlots();
    Console::info(TAG, "Gardener démarré — "
                  + String(gardenerWateringSlotCount) + " créneau(x) chargé(s)");
}

// ─── handle() — tâche périodique 1000 ms ─────────────────────────────────────

void GardenerManager::handle()
{
    // 1. VClock disponible ? Sinon, ni scheduler ni traitement MQTT.
    TimeVClock t = VirtualClock::read();
    if (!t.VClock_available) return;

    // 2. Traiter le buffer MQTT entrant (FromUser) si présent.
    if (gardenerMsgPending) {
        gardenerMsgPending = false;
        processGardenerMessage();
    }

    // 3. Obtenir l'heure locale courante.
    time_t ts = (time_t)t.timestamp;
    struct tm tmLocal;
    localtime_r(&ts, &tmLocal);

    uint16_t currentMinute = (uint16_t)(tmLocal.tm_hour * 60 + tmLocal.tm_min);

    // 4. Même minute déjà traitée ? → rien à faire.
    if (currentMinute == gardenerLastMinute) return;

    // 5. Parcourir les créneaux et déclencher ceux qui correspondent.
    for (uint8_t i = 0; i < gardenerWateringSlotCount; i++) {
        const GardenerWateringSlot& slot = gardenerWateringSlots[i];
        if ((uint16_t)(slot.hour * 60 + slot.minute) == currentMinute) {
            BusItem item = {};
            item.type       = DataType::CommandAuto;
            item.id         = slot.cmdId;
            item.valueKind  = 0;
            item.valueFloat = (float)slot.duration;
            DataBus::publish(item);

            Console::info(TAG, "Arrosage auto cmdId="
                          + String((uint8_t)slot.cmdId)
                          + " — durée " + String(slot.duration) + " s");
        }
    }

    // 6. Mémoriser la minute courante (anti-rebond).
    gardenerLastMinute = currentMinute;
}

// ─── onGardenerMessage() — thread esp_mqtt ───────────────────────────────────
// Bufferise le message brut pour traitement dans handle() (thread TaskManager).
// Si un nouveau message arrive avant traitement, il écrase le précédent.

void GardenerManager::onGardenerMessage(const char* data, int len)
{
    if (len <= 0 || (size_t)len >= MSG_BUFFER_SIZE) {
        return;
    }
    memcpy(gardenerMsgBuffer, data, len);
    gardenerMsgBuffer[len] = '\0';
    gardenerMsgPending = true;
}

// ─── processGardenerMessage() — thread TaskManager ───────────────────────────
// Parse le JSON FromUser et exécute l'opération add ou remove.
// Publie l'état courant (ToUser) dans tous les cas.

void GardenerManager::processGardenerMessage()
{
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, gardenerMsgBuffer);
    if (err) {
        Console::warn(TAG, "JSON FromUser malformé : " + String(err.c_str()));
        publishGardenerWateringState();
        return;
    }

    const char* op = doc["op"] | "";

    if (strcmp(op, "add") == 0) {
        GardenerWateringSlot slot;
        slot.cmdId              = (DataId)(uint8_t)(doc["cmdId"] | 0);
        slot.hour               = doc["hour"] | 0;
        slot.minute             = doc["minute"] | 0;
        slot.duration           = doc["duration"] | 0;
        slot.cancellableBySensor = doc["cancellableBySensor"] | false;

        addGardenerWateringSlot(slot);

    } else if (strcmp(op, "remove") == 0) {
        DataId cmdId  = (DataId)(uint8_t)(doc["cmdId"] | 0);
        uint8_t hour   = doc["hour"] | 0;
        uint8_t minute = doc["minute"] | 0;

        removeGardenerWateringSlot(cmdId, hour, minute);

    } else {
        Console::warn(TAG, "op inconnu : " + String(op));
    }

    publishGardenerWateringState();
}

// ─── validateGardenerWateringSlot() ──────────────────────────────────────────
// Vérifie les bornes des champs et que cmdId est une commande routée par RELAYS[].

bool GardenerManager::validateGardenerWateringSlot(const GardenerWateringSlot& slot)
{
    // cmdId doit exister dans META comme CommandGeneric
    if (!isValidId((uint8_t)slot.cmdId)) {
        Console::warn(TAG, "cmdId=" + String((uint8_t)slot.cmdId) + " inconnu de META");
        return false;
    }
    const DataMeta& meta = getMeta(slot.cmdId);
    if (meta.type != DataType::CommandGeneric) {
        Console::warn(TAG, "cmdId=" + String((uint8_t)slot.cmdId)
                      + " n'est pas une commande (type="
                      + String((uint8_t)meta.type) + ")");
        return false;
    }

    // cmdId doit correspondre à un relais dans RELAYS[] (source de vérité IO-Config)
    bool found = false;
    for (size_t i = 0; i < RELAYS_COUNT; i++) {
        if (RELAYS[i].command == slot.cmdId) { found = true; break; }
    }
    if (!found) {
        Console::warn(TAG, "cmdId=" + String((uint8_t)slot.cmdId)
                      + " absent de RELAYS[]");
        return false;
    }

    // Bornes horaires
    if (slot.hour > 23) {
        Console::warn(TAG, "hour=" + String(slot.hour) + " hors bornes");
        return false;
    }
    if (slot.minute > 59) {
        Console::warn(TAG, "minute=" + String(slot.minute) + " hors bornes");
        return false;
    }

    // Durée bornée par META (source de vérité)
    if (slot.duration < (uint16_t)meta.min || slot.duration > (uint16_t)meta.max) {
        Console::warn(TAG, "duration=" + String(slot.duration)
                      + " hors bornes META [" + String(meta.min, 0)
                      + ", " + String(meta.max, 0) + "]");
        return false;
    }

    return true;
}

// ─── hasGardenerTimeOverlap() ────────────────────────────────────────────────
// Vérifie si un nouveau créneau chevauche un créneau existant sur la même vanne.
// Un créneau occupe les minutes [start, start + ceil(duration/60) - 1] modulo 1440.

bool GardenerManager::hasGardenerTimeOverlap(const GardenerWateringSlot& newSlot)
{
    uint16_t newStart = newSlot.hour * 60 + newSlot.minute;
    uint16_t newSpan  = (newSlot.duration + 59) / 60;   // ceil(duration / 60)

    for (uint8_t i = 0; i < gardenerWateringSlotCount; i++) {
        const GardenerWateringSlot& s = gardenerWateringSlots[i];
        if (s.cmdId != newSlot.cmdId) continue;
        // Un doublon exact n'est pas un chevauchement (traité séparément)
        if (s.hour == newSlot.hour && s.minute == newSlot.minute) continue;

        uint16_t sStart = s.hour * 60 + s.minute;
        uint16_t sSpan  = (s.duration + 59) / 60;

        for (uint16_t m = 0; m < newSpan; m++) {
            uint16_t mine = (newStart + m) % 1440;
            for (uint16_t n = 0; n < sSpan; n++) {
                if (mine == (sStart + n) % 1440) return true;
            }
        }
    }
    return false;
}

// ─── countGardenerSlotsForValve() ────────────────────────────────────────────

uint8_t GardenerManager::countGardenerSlotsForValve(DataId cmdId)
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < gardenerWateringSlotCount; i++) {
        if (gardenerWateringSlots[i].cmdId == cmdId) count++;
    }
    return count;
}

// ─── addGardenerWateringSlot() ───────────────────────────────────────────────

bool GardenerManager::addGardenerWateringSlot(const GardenerWateringSlot& slot)
{
    // Validation des champs
    if (!validateGardenerWateringSlot(slot)) return false;

    // Saturation globale
    if (gardenerWateringSlotCount >= MAX_WATERING_SLOTS_TOTAL) {
        Console::warn(TAG, "Tableau plein (" + String(MAX_WATERING_SLOTS_TOTAL)
                      + " créneaux)");
        return false;
    }

    // Saturation par vanne
    if (countGardenerSlotsForValve(slot.cmdId) >= MAX_WATERING_SLOTS_PER_VALVE) {
        Console::warn(TAG, "Vanne cmdId=" + String((uint8_t)slot.cmdId)
                      + " déjà à " + String(MAX_WATERING_SLOTS_PER_VALVE)
                      + " créneaux");
        return false;
    }

    // Doublon exact (même cmdId + hour + minute)
    for (uint8_t i = 0; i < gardenerWateringSlotCount; i++) {
        if (gardenerWateringSlots[i].cmdId  == slot.cmdId &&
            gardenerWateringSlots[i].hour   == slot.hour  &&
            gardenerWateringSlots[i].minute == slot.minute) {
            Console::warn(TAG, "Doublon détecté : cmdId=" + String((uint8_t)slot.cmdId)
                          + " " + String(slot.hour) + ":" + String(slot.minute));
            return false;
        }
    }

    // Chevauchement temporel
    if (hasGardenerTimeOverlap(slot)) {
        Console::warn(TAG, "Chevauchement détecté : cmdId=" + String((uint8_t)slot.cmdId)
                      + " " + String(slot.hour) + ":"
                      + String(slot.minute) + " durée " + String(slot.duration) + "s");
        return false;
    }

    // Ajout
    gardenerWateringSlots[gardenerWateringSlotCount++] = slot;

    if (!saveGardenerWateringSlots()) {
        Console::error(TAG, "Échec sauvegarde après ajout");
    }

    Console::info(TAG, "Créneau ajouté : cmdId=" + String((uint8_t)slot.cmdId)
                  + " à " + String(slot.hour) + ":"
                  + String(slot.minute) + " pendant " + String(slot.duration) + " s");
    return true;
}

// ─── removeGardenerWateringSlot() ────────────────────────────────────────────

bool GardenerManager::removeGardenerWateringSlot(DataId cmdId, uint8_t hour, uint8_t minute)
{
    for (uint8_t i = 0; i < gardenerWateringSlotCount; i++) {
        if (gardenerWateringSlots[i].cmdId  == cmdId &&
            gardenerWateringSlots[i].hour   == hour  &&
            gardenerWateringSlots[i].minute == minute) {

            // Swap avec le dernier élément
            gardenerWateringSlots[i] = gardenerWateringSlots[gardenerWateringSlotCount - 1];
            gardenerWateringSlotCount--;

            if (!saveGardenerWateringSlots()) {
                Console::error(TAG, "Échec sauvegarde après suppression");
            }

            Console::info(TAG, "Créneau supprimé : cmdId=" + String((uint8_t)cmdId)
                          + " à " + String(hour) + ":" + String(minute));
            return true;
        }
    }

    Console::warn(TAG, "Créneau inexistant : cmdId=" + String((uint8_t)cmdId)
                  + " " + String(hour) + ":" + String(minute));
    return false;
}

// ─── serializeGardenerWateringSlots() ────────────────────────────────────────
// Format JSON commun à la persistance LittleFS et à la publication MQTT ToUser.

String GardenerManager::serializeGardenerWateringSlots()
{
    DynamicJsonDocument doc(4096);
    JsonArray slots = doc.createNestedArray("slots");

    for (uint8_t i = 0; i < gardenerWateringSlotCount; i++) {
        const GardenerWateringSlot& s = gardenerWateringSlots[i];
        JsonObject obj = slots.createNestedObject();
        obj["cmdId"]              = (uint8_t)s.cmdId;
        obj["hour"]               = s.hour;
        obj["minute"]             = s.minute;
        obj["duration"]           = s.duration;
        obj["cancellableBySensor"] = s.cancellableBySensor;
    }

    String result;
    serializeJson(doc, result);
    return result;
}

// ─── saveGardenerWateringSlots() ─────────────────────────────────────────────
// Écriture atomique : /gardener.tmp → rename → /gardener.json.

bool GardenerManager::saveGardenerWateringSlots()
{
    String json = serializeGardenerWateringSlots();

    File f = LittleFS.open("/gardener.tmp", "w");
    if (!f) {
        Console::error(TAG, "Échec ouverture /gardener.tmp en écriture");
        return false;
    }

    size_t written = f.print(json);
    f.close();

    if (written != json.length()) {
        Console::error(TAG, "Écriture partielle /gardener.tmp ("
                      + String(written) + "/" + String(json.length()) + ")");
        return false;
    }

    if (!LittleFS.rename("/gardener.tmp", "/gardener.json")) {
        Console::error(TAG, "Échec rename /gardener.tmp → /gardener.json");
        return false;
    }

    return true;
}

// ─── loadGardenerWateringSlots() ─────────────────────────────────────────────

bool GardenerManager::loadGardenerWateringSlots()
{
    gardenerWateringSlotCount = 0;

    File f = LittleFS.open("/gardener.json", "r");
    if (!f) {
        Console::warn(TAG, "Fichier /gardener.json absent — démarrage à vide");
        return false;
    }

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Console::error(TAG, "JSON malformé dans /gardener.json : "
                      + String(err.c_str()));
        return false;
    }

    JsonArray slots = doc["slots"];
    for (JsonObject obj : slots) {
        if (gardenerWateringSlotCount >= MAX_WATERING_SLOTS_TOTAL) {
            Console::warn(TAG, "Plus de " + String(MAX_WATERING_SLOTS_TOTAL)
                          + " créneaux dans le fichier — surplus ignoré");
            break;
        }

        GardenerWateringSlot slot;
        slot.cmdId              = (DataId)(uint8_t)(obj["cmdId"] | 0);
        slot.hour               = obj["hour"] | 0;
        slot.minute             = obj["minute"] | 0;
        slot.duration           = obj["duration"] | 0;
        slot.cancellableBySensor = obj["cancellableBySensor"] | false;

        if (validateGardenerWateringSlot(slot)) {
            gardenerWateringSlots[gardenerWateringSlotCount++] = slot;
        } else {
            Console::warn(TAG, "Créneau ignoré au chargement (invalide)");
        }
    }

    return true;
}

// ─── publishGardenerWateringState() ──────────────────────────────────────────
// Sérialise l'état courant et publie via MqttManager (retain sur ToUser).

void GardenerManager::publishGardenerWateringState()
{
    String json = serializeGardenerWateringSlots();
    MqttManager::publishGardenerWateringState(json.c_str(), json.length());
}