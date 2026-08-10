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
portMUX_TYPE  GardenerManager::gardenerMsgMux = portMUX_INITIALIZER_UNLOCKED;
char          GardenerManager::gardenerMsgWork[MSG_BUFFER_SIZE] = {};

volatile bool GardenerManager::gardenerStatePublishPending = false;

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
    // 1. Publication d'état demandée depuis le thread esp_mqtt. Traitée avant
    //    le test d'horloge : la sérialisation des créneaux n'en dépend pas, et
    //    une UI qui se connecte doit recevoir l'état sans attendre VClock.
    if (gardenerStatePublishPending) {
        gardenerStatePublishPending = false;
        publishGardenerWateringState();
    }

    // 2. VClock disponible ? Sinon, ni scheduler ni traitement MQTT.
    TimeVClock t = VirtualClock::read();
    if (!t.VClock_available) return;

    // 3. Traiter le buffer MQTT entrant (FromUser) si présent.
    //    La recopie sous portMUX est indispensable : le parsing ArduinoJson
    //    est en zéro-copie et relit le buffer bien après la désérialisation.
    bool hasMsg = false;

    taskENTER_CRITICAL(&gardenerMsgMux);
    if (gardenerMsgPending) {
        memcpy(gardenerMsgWork, gardenerMsgBuffer, MSG_BUFFER_SIZE);
        gardenerMsgPending = false;
        hasMsg = true;
    }
    taskEXIT_CRITICAL(&gardenerMsgMux);

    if (hasMsg) {
        processGardenerMessage(gardenerMsgWork);
    }

    // 4. Obtenir l'heure locale courante.
    time_t ts = (time_t)t.timestamp;
    struct tm tmLocal;
    localtime_r(&ts, &tmLocal);

    uint16_t currentMinute = (uint16_t)(tmLocal.tm_hour * 60 + tmLocal.tm_min);

    // 5. Même minute déjà traitée ? → rien à faire.
    if (currentMinute == gardenerLastMinute) return;

    // 6. Parcourir les créneaux et déclencher ceux qui correspondent.
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

    // 7. Mémoriser la minute courante (anti-rebond).
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
    taskENTER_CRITICAL(&gardenerMsgMux);
    memcpy(gardenerMsgBuffer, data, len);
    gardenerMsgBuffer[len] = '\0';
    gardenerMsgPending = true;
    taskEXIT_CRITICAL(&gardenerMsgMux);
}

// ─── requestStatePublish() — thread esp_mqtt ─────────────────────────────────
// Ne publie pas : la publication parcourt gardenerWateringSlots[] et doit
// rester dans le thread TaskManager. Voir handle().

void GardenerManager::requestStatePublish()
{
    gardenerStatePublishPending = true;
}

// ─── processGardenerMessage() — thread TaskManager ───────────────────────────
// Parse le JSON FromUser et exécute l'opération add ou remove.
// Publie l'état courant (ToUser) dans tous les cas.

// msg est la copie de travail, jamais le buffer partagé avec le thread esp_mqtt.

void GardenerManager::processGardenerMessage(char* msg)
{
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, msg);
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

    // Sauvegarde impossible : l'écriture atomique garantit que /gardener.json
    // est resté intact, donc on annule l'ajout en RAM. Sans cela le créneau
    // vivrait jusqu'au prochain reboot et serait publié sur ToUser, laissant
    // croire à l'utilisateur qu'il est enregistré.
    if (!saveGardenerWateringSlots()) {
        gardenerWateringSlotCount--;
        Console::error(TAG, "Échec sauvegarde — créneau NON ajouté");
        return false;
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

            // Swap avec le dernier élément. Le créneau retiré est mémorisé
            // pour pouvoir défaire l'opération si la sauvegarde échoue.
            const uint8_t              lastIdx     = gardenerWateringSlotCount - 1;
            const GardenerWateringSlot removedSlot = gardenerWateringSlots[i];

            gardenerWateringSlots[i] = gardenerWateringSlots[lastIdx];
            gardenerWateringSlotCount--;

            // Même raisonnement que pour l'ajout : le fichier est intact, on
            // restaure l'état d'avant le swap pour que RAM et disque concordent.
            if (!saveGardenerWateringSlots()) {
                gardenerWateringSlotCount++;
                gardenerWateringSlots[lastIdx] = gardenerWateringSlots[i];
                gardenerWateringSlots[i]       = removedSlot;
                Console::error(TAG, "Échec sauvegarde — créneau NON supprimé");
                return false;
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