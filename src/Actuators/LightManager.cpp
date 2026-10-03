// Actuators/LightManager.cpp
// Manager métier "lumière" — voir LightManager.h
//
// Construction dynamique de slots[] :
//   Au premier passage de handle() après VALVE_START_DELAY_MS, le manager
//   scanne NEO et ramasse les entités dont le handler est enqueueByEntity.
//   ValveManager ne voit pas ces canaux, et réciproquement.
//
// Pilotage matériel :
//   Toute action physique passe par digitalWrite sur RELAYS[].gpio, canal par
//   canal : cette carte n'a pas d'expandeur, aucun mot de sortie n'est partagé
//   avec ValveManager. La protection immédiate au boot (GPIO forcés LOW) est
//   portée par initAllRelayPinsSafe() (IO-Config.h), appelée dans
//   main.cpp::setup().
//
// Thread-safety :
//   - Commandes MQTT dans le thread esp_mqtt, HTTP dans AsyncWebServer.
//   - Chaque dispatcher parse et valide via DataBus::parseCommand, puis
//     publie via DataBus::publish. DataBus::routeCommand consulte NEO et
//     invoque le handler qui y est stocké — pour ce module,
//     enqueueByEntity() — qui fait xQueueSend.
//   - handle() consomme via xQueueReceive dans le thread TaskManager.
//   - Aucune variable d'état n'est accédée concurremment.

#include "Actuators/LightManager.h"
#include "Core/DataBus.h"
#include "Config/IO-Config.h"   // RELAYS[].gpio — couche physique de la carte
#include "Config/Neo.h"
#include "Utils/Console.h"

static const char* TAG = "LightManager";

// -----------------------------------------------------------------------------
// État statique
// -----------------------------------------------------------------------------
LightManager::LightSlot LightManager::slots[LIGHT_MAX] = {};
uint8_t       LightManager::slotCount        = 0;
bool          LightManager::lightSystemReady = false;
QueueHandle_t LightManager::cmdQueue         = nullptr;

// -----------------------------------------------------------------------------
// Une entrée NEO appartient-elle à ce manager ?
// Le critère est le handler que RELAYS[] a inscrit sur la ligne, jamais une
// liste d'ids tenue ici : confier un canal à une lumière dans RELAYS[] suffit
// alors à ce que ce manager le voie. Le filtre sur le type écarte l'entrée de
// commande, que relayNeoAt dote du même handler que l'entité.
// -----------------------------------------------------------------------------
static bool isLightEntry(const NeoEntry& entry)
{
    return entry.relayCh != 0
        && entry.enqueue == &LightManager::enqueueByEntity
        && getMeta(entry.id).type == DataType::Actuator;
}

// -----------------------------------------------------------------------------
// Construction de slots[] par scan de NEO
// NEO porte déjà l'entité, son canal relais et sa commande liée : rien n'est
// écrit en double ici, seul l'état runtime est initialisé.
// -----------------------------------------------------------------------------
void LightManager::buildSlotsFromNeo()
{
    slotCount = 0;
    for (uint8_t i = 0; i < Neo::count(); i++) {
        const NeoEntry& entry = Neo::at(i);

        if (!isLightEntry(entry)) continue;

        // Un relais, une lumière. Deux lignes de RELAYS[] portant la même
        // entité sont une erreur de déclaration : le second canal serait
        // silencieusement muet, findSlot ne rendant jamais son slot.
        LightSlot* duplicate = nullptr;
        if (findSlot(entry.id, duplicate)) {
            Console::warn(TAG, "Entité " + String((uint8_t)entry.id) +
                          " déclarée sur plusieurs canaux — canal " +
                          String(entry.relayCh) + " ignoré");
            continue;
        }

        if (slotCount >= LIGHT_MAX) {
            Console::warn(TAG, "NEO contient plus de lumières que LIGHT_MAX — surplus ignoré");
            return;
        }
        slots[slotCount].id      = entry.id;
        slots[slotCount].relayCh = entry.relayCh;
        slots[slotCount].state   = LIGHT_OFF;
        slotCount++;
    }
}

// -----------------------------------------------------------------------------
// Recherche linéaire dans slots[] (coût négligeable)
// -----------------------------------------------------------------------------
bool LightManager::findSlot(DataId id, LightSlot*& outSlot)
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
// Scrutation périodique — unique tâche côté lumières
// -----------------------------------------------------------------------------
void LightManager::handle()
{
    // ─── Silence total avant le délai ─────────────────────────────────────
    if (millis() < VALVE_START_DELAY_MS) {
        return;
    }

    // ─── Démarrage paresseux au premier passage après le délai ────────────
    if (!lightSystemReady) {
        buildSlotsFromNeo();

        // Profondeur ajustée au nombre de lumières réellement affectées : une
        // commande en attente par lumière suffit, et zéro lumière n'est pas
        // une taille de queue valide.
        cmdQueue = xQueueCreate(slotCount > 0 ? slotCount : 1, sizeof(LightCommand));
        if (cmdQueue == nullptr) {
            Console::error(TAG, "Échec création queue FreeRTOS — commandes ignorées");
            // On ne lève pas lightSystemReady : on retentera au prochain tour.
            return;
        }

        lightSystemReady = true;
        Console::info(TAG, "Système lumières opérationnel — " +
                      String(slotCount) + " lumière(s) affectée(s)");

        for (uint8_t i = 0; i < slotCount; i++) {
            BusItem item = {};
            item.type       = getMeta(slots[i].id).type;
            item.id         = slots[i].id;
            item.valueKind  = 0;
            item.valueFloat = 0.0f;
            DataBus::publish(item);
        }
    }

    // ─── Consommation de la queue de commandes (non-bloquant) ────────────
    LightCommand cmd;
    while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
        applyRequested(cmd.id, cmd.requestedState);
    }
}

// -----------------------------------------------------------------------------
// Validation métier d'une commande dépilée (thread TaskManager)
// -----------------------------------------------------------------------------
void LightManager::applyRequested(DataId id, uint32_t requestedState)
{
    if (!lightSystemReady) {
        Console::info(TAG, "Commande ignorée : système lumières pas encore prêt");
        return;
    }

    if (requestedState != LIGHT_OFF && requestedState != LIGHT_ON) {
        Console::warn(TAG, "Commande lumière id=" + String((uint8_t)id) +
                      " ignorée : état " + String(requestedState) +
                      " (attendu 0=OFF ou 1=ON)");
        return;
    }

    LightSlot* slot = nullptr;
    if (!findSlot(id, slot)) {
        Console::warn(TAG, "Commande ignorée : DataId " +
                      String((uint8_t)id) + " inconnu ou non affecté");
        return;
    }

    uint8_t newState = (uint8_t)requestedState;
    if (slot->state == newState) {
        Console::info(TAG, "Lumière id=" + String((uint8_t)id) +
                      (newState == LIGHT_ON ? " déjà allumée" : " déjà éteinte"));
        return;
    }

    applyLightState(*slot, newState);
    Console::info(TAG, "Lumière id=" + String((uint8_t)id) +
                  (newState == LIGHT_ON ? " allumée" : " éteinte"));
}

// -----------------------------------------------------------------------------
// Point d'entrée thread-safe — invoqué par DataBus::routeCommand via le
// handler `enqueue` stocké dans RELAYS[] (Config/IO-Config.h). Appelé depuis
// le thread du dispatcher (esp_mqtt, AsyncTCP…) après parseCommand.
// -----------------------------------------------------------------------------
bool LightManager::enqueueByEntity(DataId entity, uint32_t requestedState)
{
    if (cmdQueue == nullptr) return false;

    LightCommand cmd;
    cmd.id             = entity;
    cmd.requestedState = requestedState;

    // xQueueSend non-bloquant, thread-safe FreeRTOS.
    return (xQueueSend(cmdQueue, &cmd, 0) == pdTRUE);
}

// -----------------------------------------------------------------------------
// Accesseur
// -----------------------------------------------------------------------------
bool LightManager::isReady()
{
    return lightSystemReady;
}

// -----------------------------------------------------------------------------
// Application physique d'un nouvel état + journalisation
// Pilote le GPIO directement via RELAYS[].gpio.
// -----------------------------------------------------------------------------
void LightManager::applyLightState(LightSlot& slot, uint8_t newState)
{
    slot.state = newState;

    // GPIO direct via RELAYS[]
    for (size_t i = 0; i < RELAYS_COUNT; i++) {
        if (RELAYS[i].ch == slot.relayCh) {
            digitalWrite(RELAYS[i].gpio,
                         (newState == LIGHT_ON) ? HIGH : LOW);
            break;
        }
    }

    BusItem item = {};
    item.type       = getMeta(slot.id).type;
    item.id         = slot.id;
    item.valueKind  = 0;
    item.valueFloat = (newState == LIGHT_ON) ? 1.0f : 0.0f;
    DataBus::publish(item);
}
