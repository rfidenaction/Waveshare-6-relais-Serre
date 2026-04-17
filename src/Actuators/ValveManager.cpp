// Actuators/ValveManager.cpp
// Pilote des 6 électrovannes — voir ValveManager.h

#include "Actuators/ValveManager.h"
#include "Config/IO-Config.h"
#include "Utils/Console.h"

static const char* TAG = "ValveManager";

// -----------------------------------------------------------------------------
// Broches GPIO (depuis IO-Config.h)
// -----------------------------------------------------------------------------
const uint8_t ValveManager::valvePins[VALVE_COUNT] = {
    RELAY_CH1_PIN, RELAY_CH2_PIN, RELAY_CH3_PIN,
    RELAY_CH4_PIN, RELAY_CH5_PIN, RELAY_CH6_PIN
};

uint8_t  ValveManager::valveStates   [VALVE_COUNT] = { VALVE_CLOSED };
uint32_t ValveManager::valveDeadlines[VALVE_COUNT] = { 0 };
bool     ValveManager::valveSystemReady            = false;

// -----------------------------------------------------------------------------
// Correspondance index → DataId
// -----------------------------------------------------------------------------
DataId ValveManager::valveDataId(uint8_t valveIndex)
{
    switch (valveIndex) {
        case 0: return DataId::Valve1;
        case 1: return DataId::Valve2;
        case 2: return DataId::Valve3;
        case 3: return DataId::Valve4;
        case 4: return DataId::Valve5;
        case 5: return DataId::Valve6;
        default: return DataId::Valve1;
    }
}

// -----------------------------------------------------------------------------
// Protection matérielle immédiate (appelée dans setup())
// Aucun log, aucune dépendance logicielle autre que l'API Arduino.
// -----------------------------------------------------------------------------
void ValveManager::initPinsSafe()
{
    for (uint8_t valveIndex = 0; valveIndex < VALVE_COUNT; valveIndex++) {
        pinMode(valvePins[valveIndex], OUTPUT);
        digitalWrite(valvePins[valveIndex], LOW);   // actif HIGH → LOW = fermée
    }
}

// -----------------------------------------------------------------------------
// Init logique (appelée dans loopInit après Console prête)
// -----------------------------------------------------------------------------
void ValveManager::init()
{
    for (uint8_t valveIndex = 0; valveIndex < VALVE_COUNT; valveIndex++) {
        valveStates[valveIndex]    = VALVE_CLOSED;
        valveDeadlines[valveIndex] = 0;
    }
    valveSystemReady = false;

    Console::info(TAG, "Initialisé (relais sécurisés dès setup). "
                  "Commandes acceptées dans " +
                  String(VALVE_START_DELAY_MS / 1000) + " s");
}

// -----------------------------------------------------------------------------
// Scrutation périodique (1000 ms)
// -----------------------------------------------------------------------------
void ValveManager::handle()
{
    // Bascule en ready une seule fois, après VALVE_START_DELAY_MS
    if (!valveSystemReady && millis() >= VALVE_START_DELAY_MS) {
        valveSystemReady = true;
        Console::info(TAG, "Système vannes opérationnel — commandes acceptées");

        // Publie l'état initial (fermée) des 6 vannes dans DataLogger → MQTT
        for (uint8_t valveIndex = 0; valveIndex < VALVE_COUNT; valveIndex++) {
            DataLogger::push(valveDataId(valveIndex), 0.0f);
        }
    }

    if (!valveSystemReady) return;

    // Ferme les vannes dont le timer a expiré
    uint32_t now = millis();
    for (uint8_t valveIndex = 0; valveIndex < VALVE_COUNT; valveIndex++) {
        if (valveStates[valveIndex] == VALVE_OPENED &&
            (int32_t)(now - valveDeadlines[valveIndex]) >= 0) {

            Console::info(TAG, "Vanne " + String(valveIndex + 1) +
                          " : fin du timer, fermeture");
            applyValveState(valveIndex, VALVE_CLOSED);
            valveDeadlines[valveIndex] = 0;
        }
    }
}

// -----------------------------------------------------------------------------
// Ouverture temporisée
// -----------------------------------------------------------------------------
void ValveManager::openFor(uint8_t valveIndex, uint32_t valveDurationMs)
{
    if (!valveSystemReady) {
        Console::info(TAG, "openFor ignoré : système vannes pas encore prêt");
        return;
    }

    if (valveIndex >= VALVE_COUNT) {
        Console::info(TAG, "openFor ignoré : index hors plage (" +
                      String(valveIndex) + ")");
        return;
    }

    if (valveStates[valveIndex] == VALVE_OPENED) {
        Console::info(TAG, "Vanne " + String(valveIndex + 1) +
                      " déjà ouverte, nouvelle demande ignorée");
        return;
    }

    uint32_t clampedDuration = valveDurationMs;
    if (clampedDuration > VALVE_MAX_DURATION_MS) {
        Console::info(TAG, "Durée " + String(valveDurationMs) +
                      " ms clampée à " + String(VALVE_MAX_DURATION_MS) + " ms");
        clampedDuration = VALVE_MAX_DURATION_MS;
    }

    applyValveState(valveIndex, VALVE_OPENED);
    valveDeadlines[valveIndex] = millis() + clampedDuration;

    Console::info(TAG, "Vanne " + String(valveIndex + 1) +
                  " ouverte pour " + String(clampedDuration) + " ms");
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
void ValveManager::applyValveState(uint8_t valveIndex, uint8_t newState)
{
    valveStates[valveIndex] = newState;
    digitalWrite(valvePins[valveIndex], (newState == VALVE_OPENED) ? HIGH : LOW);

    DataLogger::push(valveDataId(valveIndex),
                     (newState == VALVE_OPENED) ? 1.0f : 0.0f);
}