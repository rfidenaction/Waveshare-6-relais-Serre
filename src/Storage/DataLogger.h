// Storage/DataLogger.h
// Portage Waveshare ESP32-S3-Relay-6CH
// Changements :
//  - DataType::Battery → DataType::Power (tension alimentation 7-36V)
//  - Suppression DataId Battery (sauf nouveau SupplyVoltage)
//  - Suppression DataId Cellular (pas de modem)
//  - Renumérotation dense de l'enum DataId
//  - Correction LogFileStats : totalGB → totalMB (partition SPIFFS = 2 MB)
#pragma once

#include <Arduino.h>
#include <map>
#include <time.h>
#include <variant>  // C++17 pour gérer float et String

// ─────────────────────────────────────────────
// Référentiel temporel
//
// Ce module ne fournit jamais d'heure locale
// Toute conversion UTC → locale est externe
// ─────────────────────────────────────────────

enum class TimeBase : uint8_t {
    Relative,   // millis depuis boot
    UTC         // timestamp absolu
};

// ─────────────────────────────────────────────
// DataType — domaine / regroupement
//
// Axe "où ça appartient" (distinct de DataNature).
// Présent dans chaque enregistrement du log brut :
//   timestamp,type,id,valueType,value
// Permet filtrage et regroupement côté Python/Web.
// ─────────────────────────────────────────────

enum class DataType : uint8_t {
    Power    = 0,   // Alimentation (tension entrée 7-36V)
    Sensor   = 1,   // Capteurs environnementaux
    Actuator = 2,   // Actionneurs (relais, vannes...)
    System   = 3    // Connectivité / système
};

// ─────────────────────────────────────────────
// DataNature — traitement / sémantique
//
// Axe "comment on le traite" (distinct de DataType).
// Utilisé par Python/Web pour décider :
//   metrique → moyennable, courbe
//   etat     → discret, pas de moyenne, libellés
//   texte    → textuel, pas de calcul
// ─────────────────────────────────────────────

enum class DataNature : uint8_t {
    metrique = 0,
    etat     = 1,
    texte    = 2
};

// ─────────────────────────────────────────────
// DataId — identifiant unique de mesure
//
// CONTRAT :
// - Valeurs immuables et définitives dès la mise en production
// - Enum dense (contigu de 0 à Count-1) : ne pas insérer de valeurs
//   explicites non contiguës
// - Ajout futur = nouveau DataId en fin de liste, avant Count
// - Tout DataId scalable se termine par un index numérique (ex: Valve1)
//   pour permettre le parsing automatique côté Python
//
// NOTE : renumérotation complète par rapport à la version LilyGo
//        (pas d'historique flash à conserver sur la Waveshare)
// ─────────────────────────────────────────────

enum class DataId : uint8_t {
    // ── Alimentation (DataType::Power) ───────
    SupplyVoltage    = 0,   // Tension entrée 7-36V (pont diviseur externe à prévoir)

    // ── Capteurs (DataType::Sensor) ──────────
    AirTemperature1  = 1,
    AirHumidity1     = 2,
    SoilMoisture1    = 3,

    // ── Actionneurs (DataType::Actuator) ─────
    Valve1           = 4,

    // ── Système / WiFi (DataType::System) ────
    WifiStaEnabled   = 5,
    WifiStaConnected = 6,
    WifiApEnabled    = 7,
    WifiRssi         = 8,

    // ── Événements système (DataType::System) ─
    Boot             = 9,
    Error            = 10,

    Count            = 11   // Sentinel — toujours en dernier
};

// ─────────────────────────────────────────────
// DataMeta — métadonnées d'un DataId
//
// Source de vérité unique pour tous les exports
// et affichages (schéma JSON, pages web, Python).
//
// Champs :
//   label          : libellé FR (UTF-8, accents autorisés)
//   unit           : unité (chaîne vide si non applicable)
//   nature         : metrique / etat / texte
//   stateLabels    : tableau de libellés d'états indexé par valeur entière
//                    nullptr si nature != etat
//                    nullptr sur une position = "pas de libellé" pour ce code
//   stateLabelCount: nombre d'entrées dans stateLabels (0 si nullptr)
// ─────────────────────────────────────────────

struct DataMeta {
    const char*        label;
    const char*        unit;
    DataNature         nature;
    const char* const* stateLabels;
    uint8_t            stateLabelCount;
};

// ─────────────────────────────────────────────
// Tableaux de libellés d'états (constexpr)
// Un tableau par DataId de nature "etat"
// ─────────────────────────────────────────────

inline constexpr const char* const kLabelsWifiStaEnabled[]    = { "Inactif",       "Actif"       };
inline constexpr const char* const kLabelsWifiStaConnected[]  = { "Déconnecté",    "Connecté"    };
inline constexpr const char* const kLabelsWifiApEnabled[]     = { "Inactif",       "Actif"       };
inline constexpr const char* const kLabelsValve1[]            = { "Fermée",        "Ouverte"     };

// ─────────────────────────────────────────────
// META — table de métadonnées
//
// Indexée directement par (uint8_t)DataId::X.
// Lookup O(1), aucune recherche, aucune allocation.
//
// CONTRAT : doit rester synchronisée avec l'enum DataId.
// Toute modification de l'enum impose une mise à jour ici.
// ─────────────────────────────────────────────

inline constexpr DataMeta META[(uint8_t)DataId::Count] = {
    // ── Alimentation ─────────────────────────────────────────────────────────
    /* SupplyVoltage  (0) */ { "Tension alim",        "V",   DataNature::metrique, nullptr,                    0 },

    // ── Capteurs ─────────────────────────────────────────────────────────────
    /* AirTemperature1 (1) */ { "Température air 1",  "°C",  DataNature::metrique, nullptr,                    0 },
    /* AirHumidity1    (2) */ { "Humidité air 1",     "%",   DataNature::metrique, nullptr,                    0 },
    /* SoilMoisture1   (3) */ { "Humidité sol 1",     "%",   DataNature::metrique, nullptr,                    0 },

    // ── Actionneurs ───────────────────────────────────────────────────────────
    /* Valve1          (4) */ { "Vanne 1",             "",    DataNature::etat,     kLabelsValve1,              2 },

    // ── Système / WiFi ────────────────────────────────────────────────────────
    /* WifiStaEnabled  (5) */ { "WiFi STA",            "",    DataNature::etat,     kLabelsWifiStaEnabled,      2 },
    /* WifiStaConnected(6) */ { "WiFi connexion",      "",    DataNature::etat,     kLabelsWifiStaConnected,    2 },
    /* WifiApEnabled   (7) */ { "Point d'accès",       "",    DataNature::etat,     kLabelsWifiApEnabled,       2 },
    /* WifiRssi        (8) */ { "WiFi RSSI",           "dBm", DataNature::metrique, nullptr,                    0 },

    // ── Événements système ────────────────────────────────────────────────────
    /* Boot   (9)  */          { "Démarrage",          "",    DataNature::texte,    nullptr,                    0 },
    /* Error  (10) */          { "Erreur",             "",    DataNature::texte,    nullptr,                    0 },
};

// ─────────────────────────────────────────────
// Enregistrement
// ─────────────────────────────────────────────

struct DataRecord {
    uint32_t timestamp;   // millis ou UTC selon timeBase
    TimeBase timeBase;
    DataType type;
    DataId   id;
    std::variant<float, String> value;  // Peut être float OU String
};

// ─────────────────────────────────────────────
// Dernière observation exposée au Web
// ─────────────────────────────────────────────
// NOTE :
// - value peut contenir soit un float, soit un String (std::variant)
// - t_rel_ms est valide uniquement si utc_valid == false
// - si utc_valid == true, seul t_utc doit être utilisé
// ─────────────────────────────────────────────

struct LastDataForWeb {
    std::variant<float, String> value;
    uint32_t  t_rel_ms  = 0;
    time_t    t_utc     = 0;
    bool      utc_valid = false;
};

// ─────────────────────────────────────────────
// Statistiques fichier de logs
// ─────────────────────────────────────────────

struct LogFileStats {
    bool   exists;
    size_t sizeBytes;
    float  sizeMB;
    float  percentFull;
    float  totalMB;     // Partition SPIFFS : 2 MB
};

// ─────────────────────────────────────────────
// DataLogger
// ─────────────────────────────────────────────

class DataLogger {
public:
    static void init();

    // Push pour valeurs numériques (float)
    static void push(DataType type, DataId id, float value);

    // Push pour valeurs textuelles (String)
    static void push(DataType type, DataId id, const String& textValue);

    static bool getLast(DataId id, DataRecord& out);

    static void handle();           // Réparation UTC + flush

    static void clearHistory();     // Supprime l'historique flash + réinitialise buffers

    // ───────────── Web ─────────────
    static bool hasLastDataForWeb(DataId id, LastDataForWeb& out);
    static bool getLastUtcRecord(DataId id, DataRecord& out);
    static String getCurrentValueWithTime(DataId id);   // LEGACY
    static String getGraphCsv(DataId id, uint32_t daysBack = 30);

    // Statistiques du fichier de logs
    static LogFileStats getLogFileStats();

    // Accès aux métadonnées d'un DataId
    // Point d'entrée unique : évite le couplage direct sur META depuis l'extérieur
    static const DataMeta& getMeta(DataId id)
    {
        return META[(uint8_t)id];
    }

private:
    // ───────────── Temps ─────────────
    static uint32_t nowRelative();

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
};