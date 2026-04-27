// Config/MetaDataModel.h
// Référentiel unique du modèle de données du système.
//
// Contient toutes les définitions partagées entre les modules :
//   DataType, DataNature, DataId  — enums
//   DataMeta, META[], META_COUNT  — métadonnées constexpr
//   DataRecord, LastDataForWeb    — structs de transport
//   Fonctions utilitaires         — getMeta, isValidId, findMetaIndex,
//                                   typeLabel, jsonEscape, escapeCSV
//
// Ce fichier ne contient AUCUNE logique d'exécution, AUCUNE queue,
// AUCUN buffer. Il décrit le système, il ne le fait pas tourner.
//
// Historique : extrait de Storage/DataLogger.h lors du refactor DataBus.
#pragma once

#include <Arduino.h>
#include <variant>
#include <time.h>

// ═════════════════════════════════════════════════════════════════════════════
// DataType — domaine fonctionnel
//
// Invariant META vs CSV/MQTT :
//   Certaines valeurs décrivent des ENTITÉS (elles apparaissent dans META) ;
//   d'autres qualifient des ÉVÉNEMENTS (elles n'apparaissent que dans le champ
//   `type` d'un DataRecord écrit en CSV ou publié sur MQTT). Les deux ensembles
//   sont disjoints.
//
//     Power, Sensor, Actuator, System   → META ET records (entité = événement)
//     CommandGeneric                    → META uniquement (jamais dans un record)
//     CommandManual, CommandAuto        → records uniquement (jamais dans META)
//
//   Les records de commande portent l'origine (Manuelle/Auto) dans `record.type`
//   et l'identité de la commande dans `record.id` (ex. CommandValve1). META.type
//   de ces entités vaut CommandGeneric — pas d'origine figée côté description.
// ═════════════════════════════════════════════════════════════════════════════

enum class DataType : uint8_t {
    Power          = 0,
    Sensor         = 1,
    Actuator       = 2,
    System         = 3,
    CommandGeneric = 4,
    CommandManual  = 5,
    CommandAuto    = 6
};

// ═════════════════════════════════════════════════════════════════════════════
// DataNature — instruction de traitement pour l'affichage
//   metrique → valeur + unité, moyennable, courbe, utilise min/max
//   etat     → libellé d'état discret, pas de moyenne
//   texte    → texte brut, pas de calcul
// ═════════════════════════════════════════════════════════════════════════════

enum class DataNature : uint8_t {
    metrique = 0,
    etat     = 1,
    texte    = 2
};

// ═════════════════════════════════════════════════════════════════════════════
// Tableaux de libellés d'états (constexpr)
// Déclarés AVANT DATA_ID_LIST car référencés par la macro
// ═════════════════════════════════════════════════════════════════════════════

inline constexpr const char* const valve1StateLabels[] = { "Fermée", "Ouverte" };
inline constexpr const char* const valve2StateLabels[] = { "Fermée", "Ouverte" };
inline constexpr const char* const valve3StateLabels[] = { "Fermée", "Ouverte" };
inline constexpr const char* const valve4StateLabels[] = { "Fermée", "Ouverte" };
inline constexpr const char* const valve5StateLabels[] = { "Fermée", "Ouverte" };
inline constexpr const char* const valve6StateLabels[] = { "Fermée", "Ouverte" };

inline constexpr const char* const kLabelsWifiStaConnected[] = { "Déconnecté",  "Connecté" };
inline constexpr const char* const kLabelsWifiApEnabled[]    = { "Inactif",     "Actif"    };

// ═════════════════════════════════════════════════════════════════════════════
// DATA_ID_LIST — SOURCE DE VÉRITÉ UNIQUE
//
// Chaque ligne définit une donnée du système. Le préprocesseur génère
// automatiquement l'enum DataId et le tableau META depuis cette liste.
//
// X(id, name, type, typeLabel, label, unit, nature, min, max, stateLabels, stateLabelCount)
//
// CONTRAT :
//   - Les valeurs numériques d'id sont IMMUABLES une fois en production
//   - Ajout d'un capteur = ajout d'UNE SEULE LIGNE dans DATA_ID_LIST
//   - L'enum DataId et META sont générés automatiquement, jamais édités à la main
//   - Le format CSV LittleFS n'est PAS impacté par les modifications de META
//
// ORDRE : les entrées sont triées par id croissant dans le tableau META.
// Les nouveaux ids sont ajoutés EN FIN de liste.
// ═════════════════════════════════════════════════════════════════════════════

#define DATA_ID_LIST \
    /* ── Alimentation ──────────────────────────────────────────────────────── */ \
    X( 0, SupplyVoltage,    Power,    "Alimentation", "Tension alim",       "V",   metrique,    5.0f,  40.0f, nullptr,                0) \
    \
    /* ── Capteurs ──────────────────────────────────────────────────────────── */ \
    X( 1, AirTemperature1,  Sensor,   "Capteur",      "Température air 1",  "°C",  metrique,  -20.0f,  60.0f, nullptr,                0) \
    X( 2, AirHumidity1,     Sensor,   "Capteur",      "Humidité air 1",     "%",   metrique,    0.0f, 100.0f, nullptr,                0) \
    X( 3, SoilMoisture1,    Sensor,   "Capteur",      "Humidité sol 1",     "%",   metrique,    0.0f, 100.0f, nullptr,                0) \
    \
    /* ── Actionneurs ───────────────────────────────────────────────────────── */ \
    X( 4, Valve1,           Actuator, "Actionneur",   "Vanne 1",            "",    etat,        0.0f,   0.0f, valve1StateLabels,      2) \
    \
    /* ── Système ───────────────────────────────────────────────────────────── */ \
    X( 5, WifiStaConnected, System,   "Système",      "WiFi STA",           "",    etat,        0.0f,   0.0f, kLabelsWifiStaConnected, 2) \
    X( 6, WifiApEnabled,    System,   "Système",      "WiFi AP",            "",    etat,        0.0f,   0.0f, kLabelsWifiApEnabled,    2) \
    X( 7, WifiRssi,         System,   "Système",      "WiFi RSSI",          "dBm", metrique, -100.0f,   0.0f, nullptr,                0) \
    X( 8, Boot,             System,   "Système",      "Démarrage",          "",    texte,       0.0f,   0.0f, nullptr,                0) \
    X( 9, Error,            System,   "Système",      "Erreur",             "",    texte,       0.0f,   0.0f, nullptr,                0) \
    X(10, TaskMonPeriod,    System,   "Système",      "Période mesurée",    "ms",  metrique,    0.0f, 10000.0f, nullptr,              0) \
    X(11, SmsEvent,         System,   "Système",      "SMS",                "",    texte,       0.0f,   0.0f, nullptr,                0) \
    \
    /* ── Actionneurs (suite) ───────────────────────────────────────────────── */ \
    X(12, Valve2,           Actuator, "Actionneur",   "Vanne 2",            "",    etat,        0.0f,   0.0f, valve2StateLabels,      2) \
    X(13, Valve3,           Actuator, "Actionneur",   "Vanne 3",            "",    etat,        0.0f,   0.0f, valve3StateLabels,      2) \
    X(14, Valve4,           Actuator, "Actionneur",   "Vanne 4",            "",    etat,        0.0f,   0.0f, valve4StateLabels,      2) \
    X(15, Valve5,           Actuator, "Actionneur",   "Vanne 5",            "",    etat,        0.0f,   0.0f, valve5StateLabels,      2) \
    X(16, Valve6,           Actuator, "Actionneur",   "Vanne 6",            "",    etat,        0.0f,   0.0f, valve6StateLabels,      2) \
    \
    /* ── Commandes (entités META, type=CommandGeneric). Records portent      */ \
    /*    CommandManual|CommandAuto dans record.type, jamais CommandGeneric. */ \
    X(17, CommandValve1,    CommandGeneric, "Commande", "Commande vanne 1", "s",   metrique,    1.0f, 900.0f, nullptr,                0) \
    X(18, CommandValve2,    CommandGeneric, "Commande", "Commande vanne 2", "s",   metrique,    1.0f, 900.0f, nullptr,                0) \
    X(19, CommandValve3,    CommandGeneric, "Commande", "Commande vanne 3", "s",   metrique,    1.0f, 900.0f, nullptr,                0) \
    X(20, CommandValve4,    CommandGeneric, "Commande", "Commande vanne 4", "s",   metrique,    1.0f, 900.0f, nullptr,                0) \
    X(21, CommandValve5,    CommandGeneric, "Commande", "Commande vanne 5", "s",   metrique,    1.0f, 900.0f, nullptr,                0) \
    X(22, CommandValve6,    CommandGeneric, "Commande", "Commande vanne 6", "s",   metrique,    1.0f, 900.0f, nullptr,                0)

// ═════════════════════════════════════════════════════════════════════════════
// Enum DataId — généré automatiquement depuis DATA_ID_LIST
// ═════════════════════════════════════════════════════════════════════════════

enum class DataId : uint8_t {
    #define X(id, name, type, typeLabel, label, unit, nature, min, max, states, cnt) \
        name = id,
    DATA_ID_LIST
    #undef X
};

// ═════════════════════════════════════════════════════════════════════════════
// DataMeta — métadonnées d'un DataId (10 champs)
// ═════════════════════════════════════════════════════════════════════════════

struct DataMeta {
    DataId             id;
    DataType           type;
    const char*        typeLabel;
    const char*        label;
    const char*        unit;
    DataNature         nature;
    float              min;
    float              max;
    const char* const* stateLabels;
    uint8_t            stateLabelCount;
};

// ═════════════════════════════════════════════════════════════════════════════
// META — tableau constexpr généré depuis DATA_ID_LIST
// Stocké en flash programme, zéro allocation RAM
// ═════════════════════════════════════════════════════════════════════════════

inline constexpr DataMeta META[] = {
    #define X(id, name, type, typeLabel, label, unit, nature, min, max, states, cnt) \
        { DataId::name, DataType::type, typeLabel, label, unit, DataNature::nature, min, max, states, cnt },
    DATA_ID_LIST
    #undef X
};

inline constexpr size_t META_COUNT = sizeof(META) / sizeof(META[0]);

// ═════════════════════════════════════════════════════════════════════════════
// DataRecord — enregistrement horodaté
//
// VClock_available et VClock_reliable : sémantique de TimeVClock (VirtualClock.h)
// ═════════════════════════════════════════════════════════════════════════════

struct DataRecord {
    uint32_t timestamp;
    bool     VClock_available;
    bool     VClock_reliable;
    DataType type;
    DataId   id;
    std::variant<float, String> value;
};

// ═════════════════════════════════════════════════════════════════════════════
// LastDataForWeb — dernière observation exposée aux interfaces web
// ═════════════════════════════════════════════════════════════════════════════

struct LastDataForWeb {
    std::variant<float, String> value;
    time_t    timestamp        = 0;
    bool      VClock_available = false;
    bool      VClock_reliable  = false;
};

// ═════════════════════════════════════════════════════════════════════════════
// Fonctions utilitaires sur META
// ═════════════════════════════════════════════════════════════════════════════

inline const DataMeta& getMeta(DataId id)
{
    for (size_t i = 0; i < META_COUNT; i++) {
        if (META[i].id == id) return META[i];
    }
    return META[0];
}

inline bool isValidId(uint8_t idByte)
{
    for (size_t i = 0; i < META_COUNT; i++) {
        if ((uint8_t)META[i].id == idByte) return true;
    }
    return false;
}

inline int findMetaIndex(uint8_t idByte)
{
    for (size_t i = 0; i < META_COUNT; i++) {
        if ((uint8_t)META[i].id == idByte) return (int)i;
    }
    return -1;
}

// Libellé canonique d'un DataType, y compris pour les types purement
// « record » (CommandManual, CommandAuto) absents de META.
inline const char* typeLabel(DataType t)
{
    switch (t) {
        case DataType::Power:          return "Alimentation";
        case DataType::Sensor:         return "Capteur";
        case DataType::Actuator:       return "Actionneur";
        case DataType::System:         return "Système";
        case DataType::CommandGeneric: return "Commande";
        case DataType::CommandManual:  return "Commande manuelle";
        case DataType::CommandAuto:    return "Commande automatique";
    }
    return "?";
}

// Échappement JSON minimal (caractères critiques uniquement).
// Les accents et caractères UTF-8 multi-octets sont valides en JSON sans escape.
inline String jsonEscape(const char* s)
{
    String out;
    if (!s) return out;
    out.reserve(strlen(s) + 4);
    while (*s) {
        char c = *s++;
        if      (c == '"')  { out += '\\'; out += '"';  }
        else if (c == '\\') { out += '\\'; out += '\\'; }
        else if (c == '\n') { out += '\\'; out += 'n';  }
        else if (c == '\r') { out += '\\'; out += 'r';  }
        else                { out += c; }
    }
    return out;
}

// Échappe une String pour CSV : ajoute guillemets et double les guillemets internes.
inline String escapeCSV(const String& text)
{
    String escaped = "\"";
    for (size_t i = 0; i < text.length(); i++) {
        char c = text.charAt(i);
        if (c == '"') {
            escaped += "\"\"";
        } else {
            escaped += c;
        }
    }
    escaped += "\"";
    return escaped;
}
