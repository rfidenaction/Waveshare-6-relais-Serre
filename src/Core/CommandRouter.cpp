// Core/CommandRouter.cpp
// Implémentation du routeur — voir CommandRouter.h pour la doctrine.
//
// Ce module est l'UNIQUE consommateur applicatif du champ enqueue de
// RELAYS[]. Les managers, eux, ne lisent que (ch, gpio, entity) — jamais
// le champ enqueue qui ne sert qu'au routage.
#include "Core/CommandRouter.h"
#include "Config/IO-Config.h"

bool CommandRouter::route(DataId cmdId, uint32_t durationMs)
{
    for (size_t i = 0; i < RELAYS_COUNT; i++) {
        if (RELAYS[i].command == cmdId) {
            return RELAYS[i].enqueue(RELAYS[i].entity, durationMs);
        }
    }
    return false;
}
