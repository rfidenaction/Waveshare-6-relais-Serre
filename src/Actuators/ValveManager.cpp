// Actuators/ValveManager.cpp
// Pilote des 6 électrovannes — voir ValveManager.h
//
// Refonte META :
//   - Tableau unique slots[] indexé par position interne, contenant
//     (DataId, GPIO, état, deadline). Aucun "index 0..5" exposé.
//   - Toutes les fonctions publiques parlent en DataId, pas en index.
//   - findSlot(DataId) est la SEULE primitive de recherche.
//
// Silence total avant VALVE_START_DELAY_MS :
//   - Aucune init() publique : handle() fait tout au premier passage après
//     le délai (création queue + publication état initial).
//   - Avant le délai : handle() est un simple if/return, rien d'autre.
//
// Thread-safety (option B2 — queue FreeRTOS) :
//   - Les commandes MQTT arrivent dans le thread esp_mqtt.
//   - Les commandes HTTP arrivent dans le thread AsyncWebServer.
//   - Les dispatchers appellent enqueueCommand() qui fait xQueueSend (thread-safe).
//   - handle() consomme la queue via xQueueReceive dans le thread TaskManager.
//   - Aucune variable d'état n'est accédée concurremment.

#include "Actuators/ValveManager.h"
#include "Config/IO-Config.h"
#include "Utils/Console.h"

static const char* TAG = "ValveManager";

// -----------------------------------------------------------------------------
// Tableau slots[] — SEULE source de vérité DataId ↔ GPIO dans tout le code
// -----------------------------------------------------------------------------
ValveManager::ValveSlot ValveManager::slots[VALVE_COUNT] = {
    { DataId::Valve1, RELAY_CH1_PIN, VALVE_CLOSED, 0 },
    { DataId::Valve2, RELAY_CH2_PIN, VALVE_CLOSED, 0 },
    { DataId::Valve3, RELAY_CH3_PIN, VALVE_CLOSED, 0 },
    { DataId::Valve4, RELAY_CH4_PIN, VALVE_CLOSED, 0 },
    { DataId::Valve5, RELAY_CH5_PIN, VALVE_CLOSED, 0 },
    { DataId::Valve6, RELAY_CH6_PIN, VALVE_CLOSED, 0 },
};

bool          ValveManager::valveSystemReady = false;
QueueHandle_t ValveManager::cmdQueue         = nullptr;

// -----------------------------------------------------------------------------
// Recherche linéaire dans slots[] (6 éléments, coût négligeable)
// -----------------------------------------------------------------------------
bool ValveManager::findSlot(DataId id, ValveSlot*& outSlot)
{
    for (uint8_t i = 0; i < VALVE_COUNT; i++) {
        if (slots[i].id == id) {
            outSlot = &slots[i];
            return true;
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// Protection matérielle immédiate (appelée dans setup())
// Aucun log, aucune dépendance logicielle autre que l'API Arduino.
// Aucune allocation, aucune queue — strictement les GPIO.
// -----------------------------------------------------------------------------
void ValveManager::initPinsSafe()
{
    for (uint8_t i = 0; i < VALVE_COUNT; i++) {
        pinMode(slots[i].gpio, OUTPUT);
        digitalWrite(slots[i].gpio, LOW);   // actif HIGH → LOW = fermée
    }
}

// -----------------------------------------------------------------------------
// Scrutation périodique (1000 ms) — unique tâche côté vannes
//
// Silence total avant VALVE_START_DELAY_MS : un simple if/return, rien d'autre.
// Au premier passage après le délai : création paresseuse de la queue et
// publication de l'état initial des 6 vannes.
// -----------------------------------------------------------------------------
void ValveManager::handle()
{
    // ─── Silence total avant le délai ─────────────────────────────────────
    if (millis() < VALVE_START_DELAY_MS) {
        return;
    }

    // ─── Démarrage paresseux au premier passage après le délai ────────────
    if (!valveSystemReady) {
        cmdQueue = xQueueCreate(VALVE_COUNT, sizeof(ValveCommand));
        if (cmdQueue == nullptr) {
            Console::error(TAG, "Échec création queue FreeRTOS — commandes ignorées");
            // On ne lève pas valveSystemReady : on retentera au prochain tour.
            return;
        }

        valveSystemReady = true;
        Console::info(TAG, "Système vannes opérationnel — commandes acceptées");

        // Publie l'état initial (fermée) des 6 vannes dans DataLogger → MQTT
        for (uint8_t i = 0; i < VALVE_COUNT; i++) {
            DataLogger::push(slots[i].id, 0.0f);
        }
    }

    // ─── Consommation de la queue de commandes (non-bloquant) ────────────
    // Vide tout ce qui est arrivé depuis le dernier tour. Chaque commande
    // passe par openFor() qui applique clamp / anti-rebond / publication.
    ValveCommand cmd;
    while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
        openFor(cmd.id, cmd.durationMs);
    }

    // ─── Ferme les vannes dont le timer a expiré ─────────────────────────
    uint32_t now = millis();
    for (uint8_t i = 0; i < VALVE_COUNT; i++) {
        ValveSlot& s = slots[i];
        if (s.state == VALVE_OPENED && (int32_t)(now - s.deadline) >= 0) {
            Console::info(TAG, "Vanne id=" + String((uint8_t)s.id) +
                          " : fin du timer, fermeture");
            applyValveState(s, VALVE_CLOSED);
            s.deadline = 0;
        }
    }
}

// -----------------------------------------------------------------------------
// Ouverture temporisée (API publique, thread TaskManager)
// -----------------------------------------------------------------------------
void ValveManager::openFor(DataId id, uint32_t durationMs)
{
    if (!valveSystemReady) {
        Console::info(TAG, "openFor ignoré : système vannes pas encore prêt");
        return;
    }

    ValveSlot* slot = nullptr;
    if (!findSlot(id, slot)) {
        Console::warn(TAG, "openFor ignoré : DataId " +
                      String((uint8_t)id) + " inconnu");
        return;
    }

    if (slot->state == VALVE_OPENED) {
        Console::info(TAG, "Vanne id=" + String((uint8_t)id) +
                      " déjà ouverte, nouvelle demande ignorée");
        return;
    }

    uint32_t clampedDuration = durationMs;
    if (clampedDuration > VALVE_MAX_DURATION_MS) {
        Console::info(TAG, "Durée " + String(durationMs) +
                      " ms clampée à " + String(VALVE_MAX_DURATION_MS) + " ms");
        clampedDuration = VALVE_MAX_DURATION_MS;
    }

    applyValveState(*slot, VALVE_OPENED);
    slot->deadline = millis() + clampedDuration;

    Console::info(TAG, "Vanne id=" + String((uint8_t)id) +
                  " ouverte pour " + String(clampedDuration) + " ms");
}

// -----------------------------------------------------------------------------
// Point d'entrée thread-safe pour producteurs externes (esp_mqtt, HTTP, etc.)
//
// Avant VALVE_START_DELAY_MS : cmdQueue == nullptr → retour false immédiat,
// la commande est rejetée côté dispatcher (log warn).
// -----------------------------------------------------------------------------
bool ValveManager::enqueueCommand(const ValveCommand& cmd)
{
    if (cmdQueue == nullptr) return false;

    // xQueueSend avec timeout 0 : non-bloquant. Retourne pdTRUE si accepté,
    // pdFALSE si la queue est pleine. La queue FreeRTOS garantit la
    // thread-safety de bout en bout sur ESP32-S3 SMP.
    return (xQueueSend(cmdQueue, &cmd, 0) == pdTRUE);
}

// -----------------------------------------------------------------------------
// Accesseur
// -----------------------------------------------------------------------------
bool ValveManager::isReady()
{
    return valveSystemReady;
}

// -----------------------------------------------------------------------------
// Application physique d'un nouvel état + journalisation
// -----------------------------------------------------------------------------
void ValveManager::applyValveState(ValveSlot& slot, uint8_t newState)
{
    slot.state = newState;
    digitalWrite(slot.gpio, (newState == VALVE_OPENED) ? HIGH : LOW);

    DataLogger::push(slot.id,
                     (newState == VALVE_OPENED) ? 1.0f : 0.0f);
}