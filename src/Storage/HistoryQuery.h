// Storage/HistoryQuery.h
// Historique à la demande — répond à une requête utilisateur en relisant les
// journaux CSV de DataLogger et en publiant une série agrégée sur MQTT.
//
// Rôle : lire, agréger, publier. Ce module ne produit aucune donnée, n'écrit
// jamais sur la flash, et ne connaît aucun capteur. La source est le journal
// de DataLogger, seule mémoire longue du système.
//
// ─── Pourquoi relire la flash plutôt que tenir un historique en RAM ─────────
//   Le journal contient déjà 366 jours de données, au format CSV 7 champs
//   identique au payload MQTT. Un tampon RAM parallèle serait une seconde
//   source de vérité, perdue à chaque reboot, et incapable de servir les
//   valeurs texte. La relecture ne coûte que du temps, et le temps est la
//   ressource dont on dispose.
//
// ─── Priorité absolue : ne jamais retarder l'arrosage ───────────────────────
//   TaskManager est coopératif et non préemptif : tout ce qui bloque ici
//   retarde ValveManager. Le scan est donc découpé en opérations élémentaires,
//   UNE SEULE par appel de handle() : soit l'ouverture d'un fichier, soit la
//   lecture d'un bloc de SCAN_CHUNK_BYTES. Aucune boucle n'attend, aucune
//   n'itère sur un nombre d'octets inconnu. Le coût par tick est donc borné
//   par construction et connu d'avance (quelques millisecondes), sans avoir à
//   surveiller une horloge en cours de traitement.
//
//   L'ouverture est isolée du reste parce qu'elle est l'opération la plus
//   coûteuse (parcours de la chaîne CTZ de littlefs), alors qu'une lecture
//   séquentielle sur un fichier déjà ouvert est bon marché.
//
//   En dernier recours, HISTORY_SCAN_DEADLINE_MS abandonne un scan trop long :
//   la réponse part avec ce qui a été collecté et le drapeau "partial". Une
//   requête peut donc être incomplète ou annulée, jamais bloquante.
//
// ─── Lecture concurrente du fichier du jour ─────────────────────────────────
//   DataLogger garde son fichier ouvert en permanence en écriture (append).
//   Nous ouvrons NOTRE PROPRE handle en lecture seule sur le même fichier.
//
//   En littlefs, un open ne donne pas une fenêtre sur un objet partagé comme
//   en POSIX : il donne une photographie privée du fichier à cet instant.
//   Deux handles sont donc deux vues indépendantes, et le danger n'existe
//   qu'entre deux ÉCRIVAINS (le dernier qui referme écrase l'autre). Un handle
//   ouvert en lecture seule ne recommitte jamais rien : il ne peut pas abîmer
//   l'append de DataLogger.
//
//   Conséquence à connaître : notre lecteur ne verra pas ce que DataLogger
//   ajoute après l'ouverture. C'est précisément ce qu'on veut — une coupe
//   cohérente du journal à l'instant du scan. D'où l'ordre imposé :
//   DataLogger::flushNow() D'ABORD, ouverture du lecteur ENSUITE.
//
//   Règle à ne jamais enfreindre : ne rien écrire par le handle de ce module,
//   et ne pas le faire seeker. C'est la seule condition de sûreté.
//
// ─── Découplage des threads ─────────────────────────────────────────────────
//   Même principe qu'OnDemandMeasure : onRequest() tourne dans le thread
//   esp_mqtt et se limite à valider puis poser la demande dans un slot ;
//   handle() exécute dans le thread TaskManager. Aucun mutex n'est nécessaire,
//   l'exclusion est obtenue par construction.
#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include "Config/MetaDataModel.h"

class HistoryQuery {
public:
    // Convention FromUser/ToUser de Gardener, Conditional et OnDemand.
    // Canal distinct de serre/ondemand/ : une demande d'historique ne déclenche
    // aucune acquisition et ne passe par aucun module capteur, elle n'a donc
    // rien à voir avec la mesure à la demande au-delà du fait que l'utilisateur
    // en est l'origine.
    static constexpr const char* HISTORY_TOPIC_FROM_USER = "serre/history/FromUser";
    static constexpr const char* HISTORY_TOPIC_TO_USER   = "serre/history/ToUser";

    static void init();

    // Avance le scan d'une opération élémentaire. Tâche TaskManager.
    // Retourne immédiatement s'il n'y a rien en cours ni en attente.
    static void handle();

    // Point d'entrée des demandes MQTT. Appelé depuis le thread esp_mqtt :
    // valide le payload, pose la demande dans le slot, rend la main.
    // Payload JSON attendu : {"op":"history","id":N,"span":H,"rid":"xxxx"}
    //   id   : DataId existant dans META
    //   span : profondeur demandée en heures, 1 à 168
    //   rid  : identifiant de requête renvoyé tel quel dans la réponse, afin
    //          qu'un téléphone ignore une réponse destinée à un autre
    static void onRequest(const char* data, int len);

    // Un scan est en cours (fichier possiblement ouvert).
    static bool isScanning();

    // Abandonne un scan en cours et referme le fichier. À appeler avant toute
    // suppression de fichier journal : effacer un fichier qu'un handle tient
    // ouvert sort du domaine défini de littlefs.
    static void abortScan();

private:
    static constexpr const char* TAG = "History";

    // ─── Bornes de la requête ────────────────────────────────────────────
    static constexpr uint8_t  SPAN_HOURS_MAX  = 168;   // 7 jours
    static constexpr uint8_t  RID_MAX         = 8;
    static constexpr int      REQUEST_MAX_LEN = 96;

    // ─── Grille d'agrégation ─────────────────────────────────────────────
    // Une tranche de 15 min en deçà de 24 h, d'une heure au-delà. Les deux
    // profondeurs prévues par l'interface tombent ainsi sur 96 et 168 points,
    // quelle que soit la cadence d'acquisition : le payload est borné par la
    // grille, pas par le volume journalisé. Les tranches vides ne sont pas
    // émises, donc en production (une mesure par heure) une demande 24 h
    // renvoie naturellement 24 points sans gaspillage.
    static constexpr uint32_t STEP_FINE_S     = 900;
    static constexpr uint32_t STEP_COARSE_S   = 3600;
    static constexpr uint8_t  FINE_SPAN_MAX_H = 24;
    static constexpr uint16_t BUCKET_MAX      = 168;

    // ─── Listes (etat et texte) ──────────────────────────────────────────
    // Une valeur d'état ou de texte ne se moyenne pas : elle se liste. Le
    // plafond est un compromis entre la RAM immobilisée et l'utilité d'une
    // liste sur un écran de téléphone. Les entrées conservées sont les plus
    // récentes, et le drapeau "truncated" le signale à l'interface.
    static constexpr uint8_t  ENTRY_MAX = 48;
    static constexpr uint8_t  TEXT_MAX  = 72;

    // ─── Tampons ─────────────────────────────────────────────────────────
    // SCAN_CHUNK_BYTES sert deux fois, jamais simultanément : bloc de lecture
    // pendant le scan, puis payload JSON au moment de la publication. Même
    // idiome que le ping-pong de DataLogger, pour ne pas immobiliser deux fois
    // 4 Ko. Taille alignée sur le bloc flash, et compatible avec les ~4 Ko du
    // schéma MQTT déjà publiés sans difficulté.
    static constexpr size_t   SCAN_CHUNK_BYTES = 4096;

    // Une ligne CSV fait au plus 256 octets (DataLogger::serializeToActive).
    // Nom préfixé : LINE_MAX est une macro POSIX exposée par <limits.h>, tiré
    // par Arduino.h via FreeRTOS.
    static constexpr size_t   CSV_LINE_MAX = 256;

    // ─── Machine à états ─────────────────────────────────────────────────
    // Idle → Prepare → (OpenFile → ReadFile)* → Publish → Idle
    enum class State : uint8_t {
        Idle,
        Prepare,     // flushNow, calcul de la fenêtre, remise à zéro
        OpenFile,    // une tentative d'ouverture par tick
        ReadFile,    // un bloc lu et analysé par tick
        Publish      // construction et émission du payload
    };

    // Une valeur d'état ou de texte horodatée. Le tableau est maintenu trié
    // par horodatage DÉCROISSANT : l'insertion garde donc les plus récentes,
    // et l'émission peut s'arrêter sur épuisement du budget en ne perdant que
    // les plus anciennes.
    struct HistEntry {
        uint32_t ts;
        float    num;                 // etat : valeur d'état ; texte : inutilisé
        char     text[TEXT_MAX + 1];  // texte : contenu ; etat : vide
    };

    struct Bucket {
        float    sum;
        uint16_t count;
    };

    // ─── Slot de demande ─────────────────────────────────────────────────
    // Écrit par onRequest (thread esp_mqtt), consommé par handle() (thread
    // TaskManager). Les champs sont posés AVANT le drapeau, comme dans
    // OnDemandMeasure : handle() ne peut donc jamais lire une demande
    // incohérente. Une demande arrivant slot occupé est refusée explicitement,
    // ce qui borne aussi le débit des requêtes.
    static volatile bool    requestPending;
    static volatile uint8_t requestedId;
    static volatile uint8_t requestedSpanH;
    static char             requestedRid[RID_MAX + 1];

    // ─── État du scan en cours ───────────────────────────────────────────
    // state est lu par onRequest depuis le thread esp_mqtt pour refuser une
    // demande concurrente, et écrit par handle() depuis TaskManager. Une
    // lecture périmée est sans conséquence : la demande serait acceptée puis
    // simplement traitée à la fin du scan en cours, jamais en parallèle.
    static volatile State state;
    static uint32_t   scanStartMs;
    static uint8_t    scanId;
    static uint8_t    scanSpanH;
    static char       scanRid[RID_MAX + 1];
    static DataNature scanNature;
    static uint32_t   windowFrom;
    static uint32_t   windowTo;
    static uint32_t   stepSeconds;
    static uint16_t   bucketCount;
    static uint8_t    fileIdx;
    static uint8_t    fileCount;
    static File       scanFile;
    static bool       partial;
    static bool       truncated;

    // ─── Accumulateurs ───────────────────────────────────────────────────
    static Bucket    buckets[BUCKET_MAX];
    static HistEntry entries[ENTRY_MAX];
    static uint8_t   entryCount;

    // Report de déduplication des états, réarmé à chaque fichier. Les valeurs
    // d'état sont publiées périodiquement même sans changement : sans ce filtre
    // une liste de 7 jours serait faite de milliers de répétitions.
    static bool  dedupValid;
    static float dedupLast;

    // ─── Tampons ─────────────────────────────────────────────────────────
    static char   chunk[SCAN_CHUNK_BYTES];
    static char   line[CSV_LINE_MAX];
    static size_t lineLen;
    static bool   lineOverflow;

    // ─── Étapes ──────────────────────────────────────────────────────────
    static void stepPrepare();
    static void stepOpenFile();
    static void stepReadFile();
    static void stepPublish();

    static void finishFile();
    static void resetScan();

    // Analyse une ligne CSV complète et l'accumule si elle concerne l'id
    // demandé et tombe dans la fenêtre.
    static void parseLine(const char* csv, size_t len);

    // Insertion triée par horodatage décroissant, plafonnée à ENTRY_MAX.
    static void insertEntry(uint32_t ts, float num, const char* text);

    // Chemin du fichier journal du jour contenant un instant donné.
    static void buildDayPath(time_t utc, char* out, size_t outSize);

    // Réponse d'erreur immédiate (busy, badreq, noclock). Payload court,
    // publié directement depuis le thread appelant.
    static void publishError(const char* rid, uint8_t id, uint8_t spanH,
                             const char* reason);
};
