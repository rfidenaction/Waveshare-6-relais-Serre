// Actuators/ValveManager.cpp
// Manager métier "vanne" — voir ValveManager.h
//
// Construction dynamique de slots[] :
//   Au premier passage de handle() après VALVE_START_DELAY_MS, le manager
//   scanne RELAYS[] (IO-Config.h) et ramasse les canaux dont l'entity est
//   une vanne (Valve1..Valve6). Il ne voit pas les autres affectations
//   (futures lumières, ventilations...), qui seront gérées par d'autres
//   managers métier sur le même RelayManager.
//
// Pilotage matériel :
//   Toute action physique passe par RelayManager::activate/deactivate(ch).
//   Ce module ne contient plus aucun digitalWrite/pinMode. La protection
//   immédiate au boot (GPIO forcés LOW) est portée par
//   RelayManager::initPinsSafe(), appelée dans main.cpp::setup().
//
// Thread-safety (inchangée) :
//   - Commandes MQTT dans le thread esp_mqtt, HTTP dans AsyncWebServer.
//   - Les dispatchers appellent enqueueCommand() qui fait xQueueSend.
//   - handle() consomme via xQueueReceive dans le thread TaskManager.
//   - Aucune variable d'état n'est accédée concurremment.

#include "Actuators/ValveManager.h"
#include "Actuators/RelayManager.h"
#include "Config/IO-Config.h"
#include "Utils/Console.h"

static const char* TAG = "ValveManager";

// -----------------------------------------------------------------------------
// État statique
// -----------------------------------------------------------------------------
ValveManager::ValveSlot ValveManager::slots[VALVE_COUNT] = {};
uint8_t       ValveManager::slotCount        = 0;
bool          ValveManager::valveSystemReady = false;
QueueHandle_t ValveManager::cmdQueue         = nullptr;

// -----------------------------------------------------------------------------
// Un DataId est-il une vanne gérée par ce manager ?
// Le ValveManager revendique explicitement Valve1..Valve6 ; les autres entités
// présentes dans RELAYS[] (lumière, ventilation…) seront prises en charge par
// d'autres managers métier.
// -----------------------------------------------------------------------------
static bool isValveEntity(DataId id)
{
    switch (id) {
        case DataId::Valve1:
        case DataId::Valve2:
        case DataId::Valve3:
        case DataId::Valve4:
        case DataId::Valve5:
        case DataId::Valve6:
            return true;
        default:
            return false;
    }
}

// -----------------------------------------------------------------------------
// Mapping statique vanne → commande associée.
// Seul endroit du code qui fait cette correspondance ; reporté ensuite dans
// slot.commandId pour que toute la suite se fasse par consultation de slots[].
// -----------------------------------------------------------------------------
static DataId commandIdFromValveEntity(DataId valveId)
{
    switch (valveId) {
        case DataId::Valve1: return DataId::CommandValve1;
        case DataId::Valve2: return DataId::CommandValve2;
        case DataId::Valve3: return DataId::CommandValve3;
        case DataId::Valve4: return DataId::CommandValve4;
        case DataId::Valve5: return DataId::CommandValve5;
        case DataId::Valve6: return DataId::CommandValve6;
        default:             return DataId::Valve1;  // neutralisé par isValveEntity en amont
    }
}

// -----------------------------------------------------------------------------
// Construction de slots[] par scan de RELAYS[]
// Accède directement aux membres statiques slots[] et slotCount.
// -----------------------------------------------------------------------------
void ValveManager::buildSlotsFromRelays()
{
    slotCount = 0;
    for (size_t i = 0; i < RELAYS_COUNT; i++) {
        if (!isValveEntity(RELAYS[i].entity)) continue;
        if (slotCount >= VALVE_COUNT) {
            Console::warn(TAG, "RELAYS[] contient plus de vannes que VALVE_COUNT — surplus ignoré");
            return;
        }
        slots[slotCount].id        = RELAYS[i].entity;
        slots[slotCount].commandId = commandIdFromValveEntity(RELAYS[i].entity);
        slots[slotCount].relayCh   = RELAYS[i].ch;
        slots[slotCount].state     = VALVE_CLOSED;
        slots[slotCount].deadline  = 0;
        slotCount++;
    }
}

// -----------------------------------------------------------------------------
// Recherche linéaire dans slots[] (au plus 6 éléments, coût négligeable)
// -----------------------------------------------------------------------------
bool ValveManager::findSlot(DataId id, ValveSlot*& outSlot)
{
    for (uint8_t i = 0; i < slotCount; i++) {
        if (slots[i].id == id) {
            outSlot = &slots[i];
            return true;
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// Scrutation périodique (1000 ms) — unique tâche côté vannes
// -----------------------------------------------------------------------------
void ValveManager::handle()
{
    // ─── Silence total avant le délai ─────────────────────────────────────
    if (millis() < VALVE_START_DELAY_MS) {
        return;
    }

    // ─── Démarrage paresseux au premier passage après le délai ────────────
    if (!valveSystemReady) {
        buildSlotsFromRelays();

        cmdQueue = xQueueCreate(VALVE_COUNT, sizeof(ValveCommand));
        if (cmdQueue == nullptr) {
            Console::error(TAG, "Échec création queue FreeRTOS — commandes ignorées");
            // On ne lève pas valveSystemReady : on retentera au prochain tour.
            return;
        }

        valveSystemReady = true;
        Console::info(TAG, "Système vannes opérationnel — " +
                      String(slotCount) + " vanne(s) affectée(s)");

        // Publie l'état initial "Fermée" des vannes effectivement affectées
        for (uint8_t i = 0; i < slotCount; i++) {
            DataLogger::push(slots[i].id, 0.0f);
        }
    }

    // ─── Consommation de la queue de commandes (non-bloquant) ────────────
    ValveCommand cmd;
    while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
        openFor(cmd.id, cmd.durationMs);
    }

    // ─── Ferme les vannes dont le timer a expiré ─────────────────────────
    uint32_t now = millis();
    for (uint8_t i = 0; i < slotCount; i++) {
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
                      String((uint8_t)id) + " inconnu ou non affecté");
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
// -----------------------------------------------------------------------------
bool ValveManager::enqueueCommand(const ValveCommand& cmd)
{
    if (cmdQueue == nullptr) return false;

    // Non-bloquant. La queue FreeRTOS garantit la thread-safety sur ESP32-S3 SMP.
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
// Pont commande ↔ observation — consulte slots[] (source unique).
// Renvoie false si les slots ne sont pas encore construits ou si la clé est
// inconnue ; l'appelant doit traiter ce cas (typiquement : ignorer le log).
// -----------------------------------------------------------------------------
bool ValveManager::commandIdForValve(DataId valveId, DataId& out)
{
    for (uint8_t i = 0; i < slotCount; i++) {
        if (slots[i].id == valveId) {
            out = slots[i].commandId;
            return true;
        }
    }
    return false;
}

bool ValveManager::getCommandTargetFor(DataId cmdId, DataId& out)
{
    for (uint8_t i = 0; i < slotCount; i++) {
        if (slots[i].commandId == cmdId) {
            out = slots[i].id;
            return true;
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// Application physique d'un nouvel état + journalisation
// Délègue l'action matérielle au RelayManager, via le canal relais du slot.
// -----------------------------------------------------------------------------
void ValveManager::applyValveState(ValveSlot& slot, uint8_t newState)
{
    slot.state = newState;
    if (newState == VALVE_OPENED) {
        RelayManager::activate(slot.relayCh);
    } else {
        RelayManager::deactivate(slot.relayCh);
    }

    DataLogger::push(slot.id,
                     (newState == VALVE_OPENED) ? 1.0f : 0.0f);
}
