// Config/Neo.h
// NEO — inventaire du matériel réellement installé sur CETTE carte.
//
// Rapport avec META (Config/MetaDataModel.h) :
//   META est le catalogue universel. Il nomme TOUTES les données que le
//   système sait décrire, il est identique sur tous les matériels du projet,
//   et ses ids sont immuables. Une entrée META peut donc exister sans que le
//   matériel correspondant soit branché ici.
//   NEO décrit ce qui est effectivement installé sur cette carte-ci.
//
//   Les deux tables se joignent par DataId, et par lui seul. NEO ne recopie
//   aucun champ de META : il ne stocke que la clé, et le libellé, l'unité, la
//   nature et les bornes se lisent dans META au moment de s'en servir.
//
// Règle unique d'affichage :
//   un DataId sans entrée NEO n'est pas affiché par l'interface.
//   C'est ainsi qu'une entité présente dans META mais absente de ce matériel
//   (Lighting7 par exemple) disparaît sans qu'aucune ligne de META ne change.
//
// Ce que NEO porte et que META ne peut pas porter :
//   - la grandeur physique mesurée   → ce qui est mesuré
//   - l'objet de la mesure           → ce qui est concerné par la mesure
//   - l'adresse RS485 du capteur
//   - pour un actionneur, le canal relais et l'id lié (vanne ↔ commande)
//
// Ces informations étaient auparavant reconstruites au cas par cas, dans le
// firmware comme dans l'interface, en analysant le texte de META.label. Or ce
// libellé est fait pour l'affichage et pour rien d'autre : le modifier cassait
// le tri, le regroupement et le routage. NEO les déclare explicitement à la
// source, une fois, là où le capteur est décrit.
//
// Déclaration et regroupement :
//   Chaque module producteur décrit ses propres entrées (neoCount/neoAt) :
//   l'information vit là où le matériel est décrit. Neo::build() les agrège
//   une fois au démarrage dans un tableau statique unique.
//
// Ce fichier ne contient que les types et la façade du tableau. Le
// regroupement a besoin de connaître tous les modules producteurs : il vit
// dans Neo.cpp, qui n'est inclus par personne et ne peut donc créer aucun
// cycle d'inclusion.
#pragma once

#include <Arduino.h>
#include "Config/MetaDataModel.h"

// ═════════════════════════════════════════════════════════════════════════════
// Grandeur — ce qui est mesuré
//
// Aucune : l'entrée ne mesure rien (actionneur, commande, entité système).
// ═════════════════════════════════════════════════════════════════════════════

enum class Grandeur : uint8_t {
    Aucune      = 0,
    Temperature = 1,
    Humidite    = 2,
    Tension     = 3,
    NiveauPile  = 4,
    Presence    = 5,
    Debit       = 6
};

// ═════════════════════════════════════════════════════════════════════════════
// Concerne — ce qui est concerné par la mesure
//
// Batterie : accumulateur d'alimentation de la carte.
// Pile     : pile d'une sonde sans fil, distincte de la batterie système.
// ═════════════════════════════════════════════════════════════════════════════

enum class Concerne : uint8_t {
    Aucun    = 0,
    Sol      = 1,
    Serre    = 2,
    Boitier  = 3,
    Batterie = 4,
    Pile     = 5,
    Secteur  = 6
};

// ═════════════════════════════════════════════════════════════════════════════
// Libellés canoniques des deux énumérations
//
// Publiés en légende avec la table, comme typeLabel() l'est pour DataType :
// l'interface reçoit des codes numériques stables et le texte qui va avec,
// sans avoir à coder en dur la signification des valeurs.
// ═════════════════════════════════════════════════════════════════════════════

inline const char* grandeurLabel(Grandeur g)
{
    switch (g) {
        case Grandeur::Aucune:      return "Aucune";
        case Grandeur::Temperature: return "Température";
        case Grandeur::Humidite:    return "Humidité";
        case Grandeur::Tension:     return "Tension";
        case Grandeur::NiveauPile:  return "Niveau de pile";
        case Grandeur::Presence:    return "Présence";
        case Grandeur::Debit:       return "Débit";
    }
    return "?";
}

// Symbole court d'une grandeur, pour les messages compacts où le libellé
// complet ne tient pas. Deux textes pour deux usages, comme typeLabel().
//
// Aucun symbole n'est partagé par deux grandeurs : c'est ce qui permet de les
// distinguer là où l'unité seule les confondrait, une hygrométrie et une
// charge de pile s'exprimant toutes deux en %.
//
// Caractères sûrs en CSV : ni virgule, ni guillemet.
inline const char* grandeurSymbol(Grandeur g)
{
    switch (g) {
        case Grandeur::Aucune:      return "-";
        case Grandeur::Temperature: return "°C";
        case Grandeur::Humidite:    return "%";
        case Grandeur::Tension:     return "V";
        case Grandeur::NiveauPile:  return "Pile";
        case Grandeur::Presence:    return "Prés";
        case Grandeur::Debit:       return "Débit";
    }
    return "?";
}

inline const char* concerneLabel(Concerne c)
{
    switch (c) {
        case Concerne::Aucun:    return "Aucun";
        case Concerne::Sol:      return "Sol";
        case Concerne::Serre:    return "Serre";
        case Concerne::Boitier:  return "Boîtier";
        case Concerne::Batterie: return "Batterie";
        case Concerne::Pile:     return "Pile";
        case Concerne::Secteur:  return "Secteur";
    }
    return "?";
}

// ═════════════════════════════════════════════════════════════════════════════
// NeoMeasure — une grandeur déclarée par un module capteur
//
// Brique de déclaration commune aux descripteurs des modules : chaque registre
// lu est décrit explicitement, sans règle implicite du genre « la première
// valeur est toujours l'humidité du sol » qu'il faudrait reconstruire ailleurs.
// ═════════════════════════════════════════════════════════════════════════════

struct NeoMeasure {
    DataId   id;
    Grandeur grandeur;
    Concerne concerne;
};

// ═════════════════════════════════════════════════════════════════════════════
// Signatures communes aux modules producteurs
// ═════════════════════════════════════════════════════════════════════════════

// Mesure immédiate d'une donnée. Retourne true si la mesure a été publiée.
using NeoMeasureFn = bool (*)(DataId id);

// Empilage d'une commande vers le manager propriétaire. Retourne true si
// acceptée. Signature identique à RelayEnqueueFn (Config/IO-Config.h).
using NeoEnqueueFn = bool (*)(DataId entity, uint32_t durationMs);

// ═════════════════════════════════════════════════════════════════════════════
// NeoEntry — une donnée réellement installée
//
// Champs valides selon le genre d'entrée :
//   capteur    → grandeur, concerne, rs485Address, measure
//   actionneur → relayCh, idLie, enqueue
//   système    → id seul, les autres champs sont neutres
//
// relayCh sert de témoin de validité à idLie et enqueue : DataId n'ayant pas
// de valeur « aucune », c'est relayCh non nul qui signale une entrée
// d'actionneur. L'invariant de RELAYS[] garantit qu'un canal relais porte
// toujours une commande et un handler.
// ═════════════════════════════════════════════════════════════════════════════

struct NeoEntry {
    DataId       id;            // clé de jointure avec META
    Grandeur     grandeur;      // ce qui est mesuré
    Concerne     concerne;      // ce qui est concerné par la mesure
    uint8_t      rs485Address;  // adresse Modbus, 0 si sans objet
    uint8_t      relayCh;       // canal relais 1-based, 0 si sans objet
    DataId       idLie;         // vanne ↔ commande, valide si relayCh != 0
    NeoMeasureFn measure;       // mesure à la demande, nullptr si sans objet
    NeoEnqueueFn enqueue;       // empilage de commande, nullptr si sans objet
};

// ═════════════════════════════════════════════════════════════════════════════
// Table NEO
// ═════════════════════════════════════════════════════════════════════════════

namespace Neo {

// Borne du tableau statique. Au plus une entrée par DataId, donc META_COUNT
// suffit par construction : le dépassement est impossible, et la borne suit
// automatiquement toute ligne ajoutée à DATA_ID_LIST.
inline constexpr size_t MAX = META_COUNT;

// Interroge les modules producteurs et remplit la table. À appeler une seule
// fois au démarrage, après les init() des modules et avant leurs premiers
// consommateurs.
void build();

// Nombre d'entrées effectivement déclarées.
uint8_t count();

// Entrée numéro `index`, avec index < count(). Hors bornes, retourne la
// première entrée plutôt que de déréférencer n'importe quoi.
const NeoEntry& at(uint8_t index);

// Entrée portant ce DataId, ou nullptr si cet id n'est pas installé sur ce
// matériel. C'est le test de la règle d'affichage.
const NeoEntry* find(DataId id);

}  // namespace Neo
