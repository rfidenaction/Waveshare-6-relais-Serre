// Storage/DataLogger.h
// Source de vérité unique pour toutes les données du système
//
// Architecture :
//   DATA_ID_LIST    → macro X qui génère l'enum DataId ET le tableau META
//   DataMeta        → struct 10 champs décrivant chaque donnée
//   META[]          → tableau constexpr en flash, organisé par id croissant
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
//   Chaque enregistrement porte deux booléens alimentés par VirtualClock :
//     VClock_available : true = timestamp est un temps UTC
//     VClock_reliable  : true = source synchronisée < 24h, false sinon
#pragma once

#include <Arduino.h>
#include <array>
#include <time.h>
#include <variant>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

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
    Power          = 0,   // Alimentation            (META + records)
    Sensor         = 1,   // Capteurs                (META + records)
    Actuator       = 2,   // Actionneurs             (META + records)
    System         = 3,   // Connectivité / système  (META + records)
    CommandGeneric = 4,   // Entité « commande »     (META uniquement)
    CommandManual  = 5,   // Record : commande manuelle  (records uniquement)
    CommandAuto    = 6    // Record : commande auto      (records uniquement)
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

inline constexpr const char* const kLabelsWifiStaConnected[] = { "Déconnecté",  "Connecté" };
inline constexpr const char* const kLabelsWifiApEnabled[]    = { "Inactif",     "Actif"    };

inline constexpr const char* const valve2StateLabels[] = { "Fermée", "Ouverte" };
inline constexpr const char* const valve3StateLabels[] = { "Fermée", "Ouverte" };
inline constexpr const char* const valve4StateLabels[] = { "Fermée", "Ouverte" };
inline constexpr const char* const valve5StateLabels[] = { "Fermée", "Ouverte" };
inline constexpr const char* const valve6StateLabels[] = { "Fermée", "Ouverte" };

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
// Trié par id croissant — les nouveaux ids sont ajoutés en fin de liste
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
// VClock_available et VClock_reliable : sémantique de TimeVClock (VirtualClock.h)
// ═════════════════════════════════════════════════════════════════════════════

struct DataRecord {
    uint32_t timestamp;          // UTC si VClock_available, millis() sinon
    bool     VClock_available;   // true = timestamp est un temps UTC
    bool     VClock_reliable;    // true = source synchronisée < 24h
    DataType type;
    DataId   id;
    std::variant<float, String> value;
};

// ═════════════════════════════════════════════════════════════════════════════
// Dernière observation exposée au Web
// ═════════════════════════════════════════════════════════════════════════════

struct LastDataForWeb {
    std::variant<float, String> value;
    time_t    timestamp        = 0;
    bool      VClock_available = false;
    bool      VClock_reliable  = false;
};

// ═════════════════════════════════════════════════════════════════════════════
// Statistiques d'utilisation de la flash
//
// Source d'information unique pour les trois canaux de présentation :
//   - Console série (logFlashUsage() au boot)
//   - Page web /logs (PageLogs::getHtml)
//   - MQTT et futur serveur HTTP (publication à venir, hors périmètre actuel) :
//     les champs en octets bruts permettent au consommateur distant de calculer
//     ses propres seuils d'alerte sans dépendance au formatage firmware.
//
// Tous les champs sont en octets, sauf indication contraire. Le formatage en
// MB et en pourcentages est fait au moment de l'affichage par chaque canal.
// ═════════════════════════════════════════════════════════════════════════════

struct FlashUsageStats {
    bool   mounted;              // SPIFFS opérationnelle (false = SPIFFS non montée)
    size_t flashTotalBytes;      // Taille flash physique (puce, ex. 16 MB)
    size_t appPartitionBytes;    // Taille de la partition app
    size_t appUsedBytes;         // Programme utilisé dans la partition app
    size_t spiffsPartitionBytes; // Taille de la partition SPIFFS
    size_t spiffsUsedBytes;      // Occupation totale SPIFFS (datalog + tout autre fichier)
    size_t datalogFileBytes;     // Taille du fichier /datalog.csv seul.
                                 // Champ dédié à la barre de progression du
                                 // téléchargement (PageLogs JS), conservé pour
                                 // ne rien casser dans le flux d'export existant.
};

// ═════════════════════════════════════════════════════════════════════════════
// DataLogger
// ═════════════════════════════════════════════════════════════════════════════

class DataLogger {
public:
    static void init();

    // ───────────── Parseur de commande (fonction pure) ────────────────────
    //
    // Pipeline dispatcher (MqttManager / WebServer) :
    //     ParsedCommand p;
    //     auto r = DataLogger::parseCommand(csv, len, p);
    //     if (r != CommandParseResult::OK) { /* rejet */ }
    //     DataLogger::submitCommand(p);            // journalisation
    //     CommandRouter::route(p.cmdId, p.durationMs);  // exécution
    //
    // Chaque étape a une responsabilité disjointe : parseCommand ne fait que
    // valider et décoder (pas d'effet de bord), submitCommand journalise via
    // le LogBufferIn (best-effort), CommandRouter exécute via RELAYS[].

    // Résultat de parseCommand. OK = CSV conforme + contenu valide. Tous les
    // autres codes sont des motifs de rejet disjoints.
    enum class CommandParseResult : uint8_t {
        OK            = 0,
        BadFormat,          // Nombre de virgules incorrect ou CSV tronqué
        TimestampSet,       // 3 premiers champs (timestamp, flags) non vides ni zéro
        InvalidType,        // type ∉ {CommandManual, CommandAuto}
        UnknownId,          // id absent de META
        NotACommand,        // META.type != CommandGeneric pour cet id
        BadValueType,       // valueType != 0 (seules les commandes float sont acceptées)
        BadValue             // value <= 0 (la durée doit être strictement positive)
    };

    // Résultat structuré de parseCommand. Renseigné seulement si retour == OK.
    //   cmdId      : entité commande validée (META.type == CommandGeneric).
    //   origin     : DataType::CommandManual ou DataType::CommandAuto.
    //   durationMs : durée en ms, dérivée du champ `value` (secondes).
    struct ParsedCommand {
        DataId   cmdId;
        DataType origin;
        uint32_t durationMs;
    };

    // Parse un CSV 7 champs "timestamp,VClock_available,VClock_reliable,type,id,valueType,value"
    // où les 3 premiers champs DOIVENT être vides ou "0" (l'émetteur n'horodate
    // pas). Fonction PURE : aucun effet de bord, aucune journalisation. En cas
    // d'OK, remplit out ; sinon out est indéfini.
    //
    //   csv, len : payload brut (MQTT data ou body HTTP), non null-terminé.
    //
    // Appelable depuis n'importe quel thread.
    static CommandParseResult parseCommand(const char* csv, size_t len,
                                           ParsedCommand& out);

    // ───────────── Entrée unifiée thread-safe (tous producteurs) ──────────
    //
    // Toute donnée produite par le système (mesure, état, événement texte,
    // trace de commande) passe par le LogBufferIn unique ci-dessous. Les trois
    // surcharges enqueue un LogBufferInItem dans la queue FreeRTOS `logBufferIn` ;
    // `handle()` draine côté TaskManager et construit les DataRecord.
    //
    // Thread-safety : appelable depuis n'importe quel thread (TaskManager,
    // esp_mqtt, AsyncTCP, future tâche automate). Non-bloquant (timeout 0
    // sur xQueueSend). En cas de saturation, l'item NEUF est perdu et un
    // warning est émis (comportement équivalent à l'historique
    // traceCommand) ; aucun producteur ne se bloque.
    //
    // Capture horloge : une seule lecture VirtualClock::read() au moment
    // de l'appel alimente LIVE et PENDING à l'identique. C'est exactement
    // ce que faisait traceCommand ; le comportement est étendu aux
    // mesures et états.
    //
    // DataType : déduit automatiquement de META pour submit(float) et
    // submit(String). Pour submitCommand, il est imposé par ParsedCommand
    // (CommandManual ou CommandAuto) — META.type vaut CommandGeneric pour
    // les entités commande et ne serait pas l'origine correcte dans le
    // record sortant.
    static void submit(DataId id, float value);
    static void submit(DataId id, const String& textValue);
    static void submitCommand(const ParsedCommand& cmd);

    // ───────────── Façades historiques (1:1 avec submit*) ─────────────────
    // Conservées pour limiter le périmètre des commits de refactor et ne
    // pas multiplier les sites d'appel à toucher. Elles délèguent sans
    // traitement supplémentaire.
    static void push(DataId id, float value)              { submit(id, value); }
    static void push(DataId id, const String& textValue)  { submit(id, textValue); }
    static void traceCommand(const ParsedCommand& cmd)    { submitCommand(cmd); }

    static void handle();           // Réparation UTC + flush

    static void clearHistory();     // Supprime l'historique flash + réinitialise buffers

    // ───────────── Sortie unifiée thread-safe (LogBufferOut) ──────────────
    //
    // Tous les records produits par applyLogBufferInItem sont déposés dans la
    // queue FreeRTOS `logBufferOut`. Les consommateurs (aujourd'hui MqttManager,
    // demain d'éventuels logger SD, écran local, etc.) la drainent à leur
    // rythme via tryPopForPublish. Politique d'éviction FIFO silencieuse
    // si saturation : les records les plus anciens sont perdus, aucun
    // producteur n'est bloqué.
    //
    // tryPopForPublish : retourne true si un record a été extrait
    // (remplit `out`), false si la queue est vide. Non-bloquant
    // (timeout 0). Appelable depuis n'importe quel thread.
    static bool tryPopForPublish(DataRecord& out);

    // ───────────── Web ─────────────
    static bool hasLastDataForWeb(DataId id, LastDataForWeb& out);
    static bool getLastUtcRecord(DataId id, DataRecord& out);

    // Statistiques d'utilisation de la flash (programme + SPIFFS).
    // Source unique pour Console, page web, et futurs canaux (MQTT, HTTP).
    static FlashUsageStats getFlashUsageStats();

    // Affiche l'état de la flash sur la console série (4 lignes encadrées).
    // Appelé une fois au boot depuis init(). Si SPIFFS n'est pas montée,
    // émet à la place un message d'erreur explicite.
    static void logFlashUsage();

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

    // Libellé canonique d'un DataType, y compris pour les types purement
    // « record » (CommandManual, CommandAuto) absents de META. Utilisé par les
    // générateurs de schéma JSON (MqttManager, WebServer).
    static const char* typeLabel(DataType t);

private:
    // ───────────── LogBufferIn (thread-safe) ───────────────
    // Unique pont entre les producteurs (quelconque thread) et le cœur de
    // DataLogger (TaskManager). Tous les submit*() enqueue ici ; handle()
    // draine côté TaskManager. Aucune primitive DataLogger (live/pending/
    // lastDataForWeb) n'est touchée hors TaskManager.
    //
    // Item POD à taille fixe : la queue FreeRTOS copie par memcpy, donc on
    // ne peut pas y stocker de std::variant<float, String> (String détient
    // un pointeur heap que le memcpy dupliquerait sans clone → double
    // free). Texte stocké dans un buffer fixe, tronqué si dépassement.
    struct LogBufferInItem {
        DataId   id;
        DataType type;            // Déduit de META pour mesure/état/texte ;
                                  // imposé par ParsedCommand pour les
                                  // commandes (CommandManual/Auto).
        uint8_t  valueKind;       // 0 = float (valueFloat), 1 = texte (valueText).
        float    valueFloat;      // Valide si valueKind == 0.
        char     valueText[200];  // Valide si valueKind == 1. Null-terminé,
                                  // tronqué si texte source > 199 caractères
                                  // (warning émis par submit(String&)).

        // Horloge unique capturée au plus tôt dans submit*() via
        // VirtualClock::read(). Alimente LIVE et PENDING à l'identique.
        // Si VClock_available=false (T < 4min), le timestamp est (uint32_t)millis()
        // et sera réparé par handle() dès que VClock bascule available.
        uint32_t timestamp;
        bool     VClock_available;
        bool     VClock_reliable;
    };
    static constexpr uint8_t LOG_BUFFER_IN_CAPACITY = 40;
    static QueueHandle_t logBufferIn;

    // Enqueue un item déjà rempli (horloges capturées en amont). Non-bloquant.
    // Si la queue est saturée ou pas encore initialisée, warning et retour
    // sans blocage — aucun producteur ne s'arrête.
    static void enqueueLogBufferIn(const LogBufferInItem& item);

    // Vide la queue et assemble les records (appelée depuis handle(),
    // thread TaskManager).
    static void drainLogBufferIn();

    // Construit LIVE + PENDING + lastDataForWeb depuis un item, dépose dans
    // le LogBufferOut.
    static void applyLogBufferInItem(const LogBufferInItem& item);

    // ───────────── LogBufferOut (thread-safe) ──────────────
    // Pont entre le cœur DataLogger (producteur unique = applyLogBufferInItem
    // sur TaskManager) et les consommateurs (MqttManager et, à terme,
    // autres subscribers). Item POD identique en esprit à LogBufferInItem mais
    // porte la valeur déjà horodatée (timestamp PENDING, UTC flags).
    struct LogBufferOutRecord {
        DataId   id;
        DataType type;
        uint32_t timestamp;           // = pendRec.timestamp (horloge VClock)
        bool     VClock_available;
        bool     VClock_reliable;
        uint8_t  valueKind;           // 0 = float, 1 = texte
        float    valueFloat;          // Valide si valueKind == 0
        char     valueText[200];      // Valide si valueKind == 1, null-terminé
    };
    static constexpr uint8_t LOG_BUFFER_OUT_CAPACITY = 20;
    static QueueHandle_t logBufferOut;

    // Dépose un record dans le LogBufferOut. Éviction FIFO silencieuse si saturé
    // (pop de la tête puis push — sans race car producteur unique sur
    // TaskManager). Appelée uniquement depuis applyLogBufferInItem.
    static void enqueueLogBufferOut(const DataRecord& rec);

    // ───────────── Buffers ─────────────
    static constexpr size_t LIVE_SIZE    = 200;
    static constexpr size_t PENDING_SIZE = 2000;
    static constexpr size_t FLUSH_SIZE   = 50;

    // LIVE (ring buffer simple)
    static DataRecord live[LIVE_SIZE];
    static size_t     liveIndex;

    // PENDING — FIFO circulaire avec perte FIFO
    static DataRecord pending[PENDING_SIZE];
    static size_t     pendingHead;
    static size_t     pendingCount;

    // ───────────── Web RAM ─────────────
    // Tableau fixe indexé par la position dans META[] (via findMetaIndex).
    // Remplace l'ancien std::map : évite toute restructuration d'arbre
    // pendant une lecture concurrente depuis la tâche AsyncTCP — le pire
    // cas restant est une lecture non atomique d'un slot, acceptable pour
    // l'affichage Web (perte de donnée tolérée, pas de crash).
    // Écritures : tâche loop() uniquement (applyLogBufferInItem depuis drainLogBufferIn, init).
    // Lectures  : hasLastDataForWeb() depuis n'importe quel thread.
    static std::array<LastDataForWeb, META_COUNT> lastDataForWeb;
    static std::array<bool,           META_COUNT> lastDataForWebHas;

    // ───────────── Internes ─────────────
    static void addLive(const DataRecord& r);
    static void addPending(const DataRecord& r);

    static void tryFlush();
    static void flushToFlash(size_t count);
};