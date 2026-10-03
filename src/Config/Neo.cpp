// Config/Neo.cpp
// Regroupement des déclarations NEO des modules producteurs.
//
// Ce fichier est le SEUL endroit qui connaît la liste des modules producteurs.
// Il les inclut tous, ce qui est sans danger : un .cpp n'est inclus par
// personne, donc aucun cycle d'inclusion n'est possible. Les modules, eux,
// n'incluent que Neo.h (les types) et ignorent l'existence de cette table.
//
// Contrôle de cohérence au remplissage :
//   - un DataId absent de META est refusé (jointure impossible)
//   - un DataId déclaré deux fois est refusé (deux modules se disputent la
//     même donnée)
// Les deux cas sont des fautes de déclaration, signalées sur Console::error.
// La publication sur DataId::Error n'est pas faite ici : build() tourne avant
// que les queues de DataBus ne soient utilisables.

#include "Config/Neo.h"
#include "Config/IO-Config.h"
#include "Utils/Console.h"

#include "Sensors/SoilSensorRS485.h"
#include "Sensors/AirSensorRS485.h"
#include "Sensors/InboxSensorRS485.h"
#include "Sensors/SupplyVoltage.h"

namespace {

constexpr const char* TAG = "NEO";

NeoEntry table[Neo::MAX] = {};
uint8_t  tableCount      = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Entités système
//
// Déclarées ici en bloc et non dans les modules : Boot et Error n'ont aucun
// module propriétaire, et les autres n'ont aucune caractéristique matérielle
// à décrire — ni grandeur, ni objet mesuré, ni adresse.
//
// Elles figurent tout de même dans NEO parce que certaines dépendent des
// modules compilés : SmsEvent n'a de sens qu'avec SmsManager, SensorHealth
// qu'avec SensorValidation, TaskMonPeriod qu'avec TaskManagerMonitor. Sur un
// matériel qui ne les compile pas, retirer la ligne suffit à les faire
// disparaître de l'interface, exactement comme pour un capteur débranché.
// ─────────────────────────────────────────────────────────────────────────────

constexpr DataId SYSTEM_IDS[] = {
    DataId::WifiStaConnected,
    DataId::WifiApEnabled,
    DataId::WifiRssi,
    DataId::Boot,
    DataId::Error,
    DataId::TaskMonPeriod,
    DataId::SmsEvent,
    DataId::SensorHealth,
};

uint8_t systemNeoCount()
{
    return sizeof(SYSTEM_IDS) / sizeof(SYSTEM_IDS[0]);
}

NeoEntry systemNeoAt(uint8_t index)
{
    if (index >= systemNeoCount()) index = 0;   // garde : index hors bornes

    NeoEntry entry = {};
    entry.id       = SYSTEM_IDS[index];
    entry.grandeur = Grandeur::Aucune;
    entry.concerne = Concerne::Aucun;

    return entry;
}

// Signature de déclaration commune à tous les modules producteurs.
using NeoAtFn = NeoEntry (*)(uint8_t index);

// Recopie les entrées d'un module dans la table, en écartant celles qui ne
// peuvent pas être jointes à META ou qui font double emploi.
void collect(uint8_t count, NeoAtFn at)
{
    for (uint8_t i = 0; i < count; i++) {
        if (tableCount >= Neo::MAX) {
            Console::error(TAG, "Table pleine (" + String((unsigned)Neo::MAX)
                              + ") — déclarations suivantes ignorées");
            return;
        }

        NeoEntry entry = at(i);

        if (findMetaIndex((uint8_t)entry.id) < 0) {
            Console::error(TAG, "DataId " + String((uint8_t)entry.id)
                              + " absent de META — entrée ignorée");
            continue;
        }

        bool duplicate = false;
        for (uint8_t j = 0; j < tableCount; j++) {
            if (table[j].id == entry.id) { duplicate = true; break; }
        }

        if (duplicate) {
            Console::error(TAG, "DataId " + String((uint8_t)entry.id)
                              + " déclaré deux fois — entrée ignorée");
            continue;
        }

        table[tableCount++] = entry;
    }
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// build — interrogation des modules producteurs
//
// L'ordre des appels fixe l'ordre de la table, sans autre conséquence : tous
// les accès se font par recherche sur DataId, jamais par position.
// ─────────────────────────────────────────────────────────────────────────────

void Neo::build()
{
    tableCount = 0;

    collect(SoilSensorRS485::neoCount(),  &SoilSensorRS485::neoAt);
    collect(AirSensorRS485::neoCount(),   &AirSensorRS485::neoAt);
    collect(InboxSensorRS485::neoCount(), &InboxSensorRS485::neoAt);
    collect(SupplyVoltage::neoCount(),    &SupplyVoltage::neoAt);
    collect(relayNeoCount(),              &relayNeoAt);
    collect(systemNeoCount(),             &systemNeoAt);

    Console::info(TAG, String(tableCount) + " entrée(s) déclarée(s) sur "
                      + String((unsigned)Neo::MAX) + " possibles");
}

// ─────────────────────────────────────────────────────────────────────────────
// Accès
// ─────────────────────────────────────────────────────────────────────────────

uint8_t Neo::count()
{
    return tableCount;
}

const NeoEntry& Neo::at(uint8_t index)
{
    if (index >= tableCount) return table[0];
    return table[index];
}

const NeoEntry* Neo::find(DataId id)
{
    for (uint8_t i = 0; i < tableCount; i++) {
        if (table[i].id == id) return &table[i];
    }
    return nullptr;
}
