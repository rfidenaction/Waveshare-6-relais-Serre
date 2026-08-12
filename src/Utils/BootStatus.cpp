// Utils/BootStatus.cpp
#include "Utils/BootStatus.h"

#include "Config/TimingConfig.h"
#include "Core/DataBus.h"
#include "Connectivity/NTPManager.h"
#include "Actuators/ValveManager.h"

char BootStatus::_message[BootStatus::MESSAGE_SIZE] = { '\0' };
bool BootStatus::_latched   = false;
bool BootStatus::_published = false;

// Protège _message/_latched : note() est appelable depuis la boucle principale
// comme depuis la tâche esp-mqtt, sur l'autre cœur.
static portMUX_TYPE bootStatusMux = portMUX_INITIALIZER_UNLOCKED;

// -----------------------------------------------------------------------------
// Mémorisation — premier arrivé, seul retenu
// -----------------------------------------------------------------------------
void BootStatus::note(const String& tag, const String& message)
{
    // Sortie anticipée : une fois le verrou posé, plus rien à faire. Évite de
    // construire une String à chaque erreur pendant toute la vie du système.
    if (_latched) {
        return;
    }

    // Composition hors section critique : aucune allocation sous portMUX.
    String full = tag.isEmpty() ? message : ("[" + tag + "] " + message);

    portENTER_CRITICAL(&bootStatusMux);
    if (!_latched) {
        strncpy(_message, full.c_str(), MESSAGE_SIZE - 1);
        _message[MESSAGE_SIZE - 1] = '\0';
        _latched = true;
    }
    portEXIT_CRITICAL(&bootStatusMux);
}

// -----------------------------------------------------------------------------
// Verdict — publication unique à l'échéance
// -----------------------------------------------------------------------------
void BootStatus::handle()
{
    if (_published) {
        return;
    }

    if (millis() < BOOT_VERDICT_DELAY_MS) {
        return;
    }

    // Critères jugeables seulement maintenant. Évalués uniquement si rien n'a
    // encore été mémorisé : une anomalie survenue plus tôt reste prioritaire.
    if (!_latched) {
        if (!NTPManager::hasEverSynced()) {
            note("NTP", "Aucune synchro NTP depuis le démarrage");
        } else if (!ValveManager::isReady()) {
            note("ValveManager", "Système vannes non opérationnel");
        }
    }

    char verdict[MESSAGE_SIZE];
    portENTER_CRITICAL(&bootStatusMux);
    if (_latched) {
        memcpy(verdict, _message, MESSAGE_SIZE);
    } else {
        strcpy(verdict, "OK");
    }
    portEXIT_CRITICAL(&bootStatusMux);

    BusItem item = {};
    item.type      = getMeta(DataId::Boot).type;
    item.id        = DataId::Boot;
    item.valueKind = 1;
    item.valueFloat = 0.0f;
    strncpy(item.valueText, verdict, sizeof(item.valueText) - 1);
    item.valueText[sizeof(item.valueText) - 1] = '\0';

    DataBus::publish(item);

    // Levé quel que soit le résultat de publish() : le verdict est un
    // instantané de fin de démarrage, il ne se rejoue pas.
    _published = true;
}
