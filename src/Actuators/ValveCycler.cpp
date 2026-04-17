// Actuators/ValveCycler.cpp
// Chenillard de test — voir ValveCycler.h

#include "Actuators/ValveCycler.h"
#include "Actuators/ValveManager.h"
#include "Utils/Console.h"

static const char* TAG = "ValveCycler";

uint8_t ValveCycler::secondsCounter = 0;

void ValveCycler::init()
{
    secondsCounter = 0;
    Console::info(TAG, "Chenillard prêt (cycle 42 s : 6 vannes × 5 s ouvertes + 2 s attente)");
}

void ValveCycler::handle()
{
    // Attend que ValveManager soit opérationnel (phase de stabilisation)
    if (!ValveManager::isReady()) return;

    // Toutes les 7 secondes (0, 7, 14, 21, 28, 35), on déclenche la vanne suivante.
    // La fermeture est gérée automatiquement par ValveManager::handle()
    // à expiration du timer de 5000 ms.
    if (secondsCounter % 7 == 0) {
        uint8_t valveIndex = secondsCounter / 7;
        ValveManager::openFor(valveIndex, 5000);
    }

    secondsCounter = (secondsCounter + 1) % 42;
}