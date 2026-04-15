// Storage/DataLogger.h
// Source de vérité unique pour toutes les données du système
//
// Architecture :
//   DATA_ID_LIST    → macro X qui génère l'enum DataId ET le tableau META
//   DataMeta        → struct 10 champs décrivant chaque donnée
//   META[]          → tableau constexpr en flash, organisé par groupes logiques
//   getMeta(DataId) → recherche linéaire sur le champ id (O(n), négligeable)
//
// CONTRAT :
//   - Les valeurs numériques d'id sont IMMUABLES une fois en production
//   - Ajout d'un capteur = ajout d'UNE SEULE LIGNE dans DATA_ID_LIST
//   - L'enum DataId et META sont générés automatiquement, jamais édités à la main
//   - Le format CSV SPIFFS n'est PAS impacté par les modifications de META
//
// Référentiel temporel :
//   Ce module ne fournit jamais d'heure locale.
//   Chaque enregistrement porte deux booléens :
//     UTC_available : true = timestamp est un temps UTC
//     UTC_reliable  : true = source RTC (précis), false = VClock (dérive)
#pragma once

#include <Arduino.h>
#include <map>
#include <time.h>
#include <variant>

// ═════════════════════════════════════════════════════════════════════════════
// DataType — domaine fonctionnel (enum manuel, 4 entrées stables)
// ═════════════════════════════════════════════════════════════════════════════

enum class DataType : uint8_t {
    Power    = 0,   // Alimentation
    Sensor   = 1,   // Capteurs environnementaux
    Actuator = 2,   // Actionneurs (relais, vannes...)
    System   = 3    // Connectivité / système
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

inline constexpr const char* const kLabelsWifiStaConnected[] = { "Déconnecté",  "Connecté" };
inline constexpr const char* const kLabelsWifiApEnabled[]    = { "Inactif",     "Actif"    };
inline constexpr const char* const kLabelsValve1[]           = { "Fermée",      "Ouverte"  };

// ═════════════════════════════════════════════════════════════════════════════
// DATA_ID_LIST — SOURCE DE VÉRITÉ UNIQUE
//
// Chaque ligne définit une donnée du système. Le préprocesseur génère
// automatiquement l'enum DataId et le tableau META depuis cette liste.
//
// X(id, name, type, typeLabel, label, unit, nature, min, max, stateLabels, stateLabelCount)
//
// Champs :
//   id             : identifiant numérique IMMUABLE (écrit dans le CSV SPIFFS)
//   name           : nom C++ pour l'enum (disparaît après compilation)
//   type           : DataType du domaine fonctionnel
//   typeLabel      : label français du domaine (pour l'affichage)
//   label          : label français de la donnée (pour l'affichage)
//   unit           : unité de mesure ("" si non applicable)
//   nature         : metrique / etat / texte
//   min            : borne basse (significatif si metrique, 0.0f sinon)
//   max            : borne haute (significatif si metrique, 0.0f sinon)
//   stateLabels    : tableau de libellés d'états (nullptr si pas etat)
//   stateLabelCount: nombre de libellés (0 si pas etat)
//
// AJOUT D'UN CAPTEUR : ajouter une ligne dans le groupe approprié.
// L'id doit être unique et ne jamais réutiliser un id existant ou passé.
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
    X( 4, Valve1,           Actuator, "Actionneur",   "Vanne 1",            "",    etat,        0.0f,   0.0f, kLabelsValve1,           2) \
    \
    /* ── Système ───────────────────────────────────────────────────────────── */ \
    X( 5, WifiStaConnected, System,   "Système",      "WiFi STA",           "",    etat,        0.0f,   0.0f, kLabelsWifiStaConnected, 2) \
    X( 6, WifiApEnabled,    System,   "Système",      "WiFi AP",            "",    etat,        0.0f,   0.0f, kLabelsWifiApEnabled,    2) \
    X( 7, WifiRssi,         System,   "Système",      "WiFi RSSI",          "dBm", metrique, -100.0f,   0.0f, nullptr,                0) \
    X( 8, Boot,             System,   "Système",      "Démarrage",          "",    texte,       0.0f,   0.0f, nullptr,                0) \
    X( 9, Error,            System,   "Système",      "Erreur",             "",    texte,       0.0f,   0.0f, nullptr,                0) \
    X(10, TaskMonPeriod,    System,   "Système",      "Période mesurée",    "ms",  metrique,    0.0f, 10000.0f, nullptr,              0) \
    X(11, SmsEvent,         System,   "Système",      "SMS",                "",    texte,       0.0f,   0.0f, nullptr,                0)

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
    const char*        typeLabel;       // Label français du domaine ("Capteur", "Système"...)
    const char*        label;           // Label français de la donnée ("Température air 1"...)
    const char*        unit;            // Unité ("°C", "%", "V"...) ou "" si non applicable
    DataNature         nature;
    float              min;             // Borne basse (significatif si metrique)
    float              max;             // Borne haute (significatif si metrique)
    const char* const* stateLabels;     // Libellés d'états (si etat), nullptr sinon
    uint8_t            stateLabelCount; // Nombre de libellés (si etat), 0 sinon
};

// ═════════════════════════════════════════════════════════════════════════════
// META — tableau constexpr généré depuis DATA_ID_LIST
// Stocké en flash programme, zéro allocation RAM
// Organisé par groupes logiques — la position n'a pas d'importance
// ═════════════════════════════════════════════════════════════════════════════

inline constexpr DataMeta META[] = {
    #define X(id, name, type, typeLabel, label, unit, nature, min, max, states, cnt) \
        { DataId::name, DataType::type, typeLabel, label, unit, DataNature::nature, min, max, states, cnt },
    DATA_ID_LIST
    #undef X
};

// Nombre d'entrées dans META (calculé automatiquement)
inline constexpr size_t META_COUNT = sizeof(META) / sizeof(META[0]);

// ═════════════════════════════════════════════════════════════════════════════
// Enregistrement (DataRecord)
//
// UTC_available et UTC_reliable : même sémantique que dans TimeUTC (ManagerUTC.h)
// ═════════════════════════════════════════════════════════════════════════════

struct DataRecord {
    uint32_t timestamp;      // UTC si UTC_available, millis() sinon
    bool     UTC_available;  // true = timestamp est un temps UTC
    bool     UTC_reliable;   // true = RTC, false = VClock ou réparé
    DataType type;
    DataId   id;
    std::variant<float, String> value;
};

// ═════════════════════════════════════════════════════════════════════════════
// Dernière observation exposée au Web
// ═════════════════════════════════════════════════════════════════════════════

struct LastDataForWeb {
    std::variant<float, String> value;
    time_t    timestamp     = 0;
    bool      UTC_available = false;
    bool      UTC_reliable  = false;
};

// ═════════════════════════════════════════════════════════════════════════════
// Statistiques fichier de logs
// ═════════════════════════════════════════════════════════════════════════════

struct LogFileStats {
    bool   exists;
    size_t sizeBytes;
    float  sizeMB;
    float  percentFull;
    float  totalMB;     // Partition SPIFFS : 2 MB
};

// ═════════════════════════════════════════════════════════════════════════════
// DataLogger
// ═════════════════════════════════════════════════════════════════════════════

class DataLogger {
public:
    static void init();

    // Push pour valeurs numériques (float)
    // DataType est déduit automatiquement de META (source de vérité unique)
    static void push(DataId id, float value);

    // Push pour valeurs textuelles (String)
    // DataType est déduit automatiquement de META (source de vérité unique)
    static void push(DataId id, const String& textValue);

    static void handle();           // Réparation UTC + flush

    static void clearHistory();     // Supprime l'historique flash + réinitialise buffers

    // ───────────── Callback publication ─────────────
    static void setOnPush(void (*callback)(const DataRecord&));

    // ───────────── Web ─────────────
    static bool hasLastDataForWeb(DataId id, LastDataForWeb& out);
    static bool getLastUtcRecord(DataId id, DataRecord& out);

    // Statistiques du fichier de logs
    static LogFileStats getLogFileStats();

    // ───────────── Accès META ─────────────
    // Recherche linéaire par DataId. O(n) avec n = META_COUNT (~10-50).
    // Négligeable sur ESP32 à 240 MHz face aux périodes de mesure.
    // Retourne META[0] en fallback (ne devrait jamais arriver).
    static const DataMeta& getMeta(DataId id)
    {
        for (size_t i = 0; i < META_COUNT; i++) {
            if (META[i].id == id) return META[i];
        }
        return META[0];
    }

    // Vérifie si un id numérique (lu depuis CSV) existe dans META
    static bool isValidId(uint8_t idByte)
    {
        for (size_t i = 0; i < META_COUNT; i++) {
            if ((uint8_t)META[i].id == idByte) return true;
        }
        return false;
    }

    // Trouve l'index dans META pour un id numérique. Retourne -1 si non trouvé.
    static int findMetaIndex(uint8_t idByte)
    {
        for (size_t i = 0; i < META_COUNT; i++) {
            if ((uint8_t)META[i].id == idByte) return (int)i;
        }
        return -1;
    }

    // ───────────── Utilitaires partagés ─────────────
    // Utilisés par DataLogger, WebServer, MqttManager
    static String jsonEscape(const char* s);
    static String escapeCSV(const String& text);

private:
    // ───────────── Buffers ─────────────
    static constexpr size_t LIVE_SIZE    = 200;
    static constexpr size_t PENDING_SIZE = 2000;
    static constexpr size_t FLUSH_SIZE   = 50;

    static constexpr uint32_t FLUSH_TIMEOUT_MS = 3600000UL; // 1 heure

    // LIVE (ring buffer simple)
    static DataRecord live[LIVE_SIZE];
    static size_t     liveIndex;

    // PENDING — FIFO circulaire avec perte FIFO
    static DataRecord pending[PENDING_SIZE];
    static size_t     pendingHead;
    static size_t     pendingCount;

    // ───────────── Web RAM ─────────────
    static std::map<DataId, LastDataForWeb> lastDataForWeb;

    // ───────────── Internes ─────────────
    static void addLive(const DataRecord& r);
    static void addPending(const DataRecord& r);

    static void tryFlush();
    static void flushToFlash(size_t count);

    // ───────────── Callback publication ─────────────
    static void (*_onPushCallback)(const DataRecord&);
};