// Actuators/RelayManager.cpp
// Driver matériel des relais — voir RelayManager.h

#include "Actuators/RelayManager.h"
#include "Config/IO-Config.h"

namespace {
    // Cache d'état logique, indexé par position dans RELAYS[].
    // Source de vérité pour isActive() — plus fiable et sémantiquement
    // plus clair que relire le GPIO en sortie.
    bool relayStates[RELAYS_COUNT] = { false };

    // Recherche linéaire du canal dans RELAYS[]. 6 entrées, négligeable.
    int findIndex(uint8_t ch) {
        for (size_t i = 0; i < RELAYS_COUNT; i++) {
            if (RELAYS[i].ch == ch) return (int)i;
        }
        return -1;
    }
}

void RelayManager::initPinsSafe()
{
    for (size_t i = 0; i < RELAYS_COUNT; i++) {
        pinMode(RELAYS[i].gpio, OUTPUT);
        digitalWrite(RELAYS[i].gpio, LOW);
        relayStates[i] = false;
    }
}

void RelayManager::activate(uint8_t ch)
{
    int i = findIndex(ch);
    if (i < 0) return;
    digitalWrite(RELAYS[i].gpio, HIGH);
    relayStates[i] = true;
}

void RelayManager::deactivate(uint8_t ch)
{
    int i = findIndex(ch);
    if (i < 0) return;
    digitalWrite(RELAYS[i].gpio, LOW);
    relayStates[i] = false;
}

bool RelayManager::isActive(uint8_t ch)
{
    int i = findIndex(ch);
    if (i < 0) return false;
    return relayStates[i];
}
