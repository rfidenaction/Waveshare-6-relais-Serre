// Storage/HistoryQuery.cpp
// Historique à la demande. Voir HistoryQuery.h pour l'architecture générale
// (une opération par tick, photographie littlefs, découplage des threads).

#include "Storage/HistoryQuery.h"
#include "Storage/DataLogger.h"
#include "Connectivity/MqttManager.h"
#include "Config/TimingConfig.h"
#include "Core/VirtualClock.h"
#include "Utils/Console.h"

#include <ArduinoJson.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ─── Variables statiques ─────────────────────────────────────────────────────
volatile bool    HistoryQuery::requestPending = false;
volatile uint8_t HistoryQuery::requestedId    = 0;
volatile uint8_t HistoryQuery::requestedSpanH = 0;
char             HistoryQuery::requestedRid[RID_MAX + 1] = {};

volatile bool HistoryQuery::archiveClearPending = false;
volatile HistoryQuery::ArchiveClearStatus HistoryQuery::archiveClearStatus =
    HistoryQuery::ArchiveClearStatus::Idle;

volatile HistoryQuery::State HistoryQuery::state = HistoryQuery::State::Idle;

uint32_t   HistoryQuery::scanStartMs = 0;
uint8_t    HistoryQuery::scanId      = 0;
uint8_t    HistoryQuery::scanSpanH   = 0;
char       HistoryQuery::scanRid[RID_MAX + 1] = {};
DataNature HistoryQuery::scanNature  = DataNature::metrique;
uint32_t   HistoryQuery::windowFrom  = 0;
uint32_t   HistoryQuery::windowTo    = 0;
uint32_t   HistoryQuery::stepSeconds = 0;
uint16_t   HistoryQuery::bucketCount = 0;
uint8_t    HistoryQuery::fileIdx     = 0;
uint8_t    HistoryQuery::fileCount   = 0;
File       HistoryQuery::scanFile;
bool       HistoryQuery::partial     = false;
bool       HistoryQuery::truncated   = false;

HistoryQuery::Bucket    HistoryQuery::buckets[BUCKET_MAX] = {};
HistoryQuery::HistEntry HistoryQuery::entries[ENTRY_MAX]  = {};
uint8_t                 HistoryQuery::entryCount          = 0;

bool  HistoryQuery::dedupValid = false;
float HistoryQuery::dedupLast  = 0.0f;

char   HistoryQuery::chunk[SCAN_CHUNK_BYTES] = {};
char   HistoryQuery::line[CSV_LINE_MAX]      = {};
size_t HistoryQuery::lineLen                 = 0;
bool   HistoryQuery::lineOverflow            = false;

// Libellé de nature publié dans la réponse. L'interface s'en sert pour choisir
// entre une courbe et une liste, sans dupliquer aucune règle sur les ids.
static const char* natureName(DataNature n)
{
    switch (n) {
        case DataNature::metrique: return "metrique";
        case DataNature::etat:     return "etat";
        case DataNature::texte:    return "texte";
    }
    return "metrique";
}

// =============================================================================
void HistoryQuery::init()
{
    resetScan();

    requestPending  = false;
    requestedId     = 0;
    requestedSpanH  = 0;
    requestedRid[0] = '\0';
    archiveClearPending = false;
    archiveClearStatus  = ArchiveClearStatus::Idle;

    Console::info(TAG, "Historique à la demande prêt — demandes sur "
                      + String(HISTORY_TOPIC_FROM_USER)
                      + ", réponses sur " + String(HISTORY_TOPIC_TO_USER));
}

// =============================================================================
bool HistoryQuery::isScanning()
{
    return state != State::Idle;
}

// =============================================================================
// Demande web de suppression. Le thread appelant ne touche à aucun handle :
// handle() annulera le scan puis supprimera une archive par tick.
// =============================================================================
bool HistoryQuery::requestArchiveClear()
{
    ArchiveClearStatus current = archiveClearStatus;
    if (current == ArchiveClearStatus::Pending ||
        current == ArchiveClearStatus::Running) {
        return false;
    }

    archiveClearStatus  = ArchiveClearStatus::Pending;
    archiveClearPending = true;
    return true;
}

HistoryQuery::ArchiveClearStatus HistoryQuery::getArchiveClearStatus()
{
    return archiveClearStatus;
}

// =============================================================================
void HistoryQuery::resetScan()
{
    if (scanFile) scanFile.close();

    state        = State::Idle;
    fileIdx      = 0;
    fileCount    = 0;
    entryCount   = 0;
    lineLen      = 0;
    lineOverflow = false;
    dedupValid   = false;
}

// =============================================================================
// Point d'entrée MQTT — thread esp_mqtt. Validation puis dépôt dans le slot.
// Aucune I/O flash, aucun traitement : la seule publication possible ici est
// une réponse d'erreur de quelques dizaines d'octets, comme le fait déjà
// handleFamilyRename sur le même événement MQTT_EVENT_DATA.
// =============================================================================
void HistoryQuery::onRequest(const char* data, int len)
{
    if (!data || len <= 0 || len > REQUEST_MAX_LEN) {
        Console::warn(TAG, "Demande d'historique de taille invalide ("
                          + String(len) + " octets)");
        return;
    }

    char buf[REQUEST_MAX_LEN + 1];
    memcpy(buf, data, (size_t)len);
    buf[len] = '\0';

    StaticJsonDocument<160> doc;
    if (deserializeJson(doc, buf) != DeserializationError::Ok) {
        Console::warn(TAG, "Demande d'historique illisible");
        return;
    }

    const char* op = doc["op"] | "";
    if (strcmp(op, "history") != 0) {
        Console::warn(TAG, "Demande d'historique : op inattendu");
        return;
    }

    // rid réduit à l'alphanumérique. Il est réémis tel quel dans la réponse :
    // le filtrer à la source dispense de l'échapper, et interdit qu'un émetteur
    // tiers casse la syntaxe du payload de réponse.
    char        rid[RID_MAX + 1] = {};
    const char* ridIn = doc["rid"] | "";
    size_t      r     = 0;
    for (size_t i = 0; ridIn[i] != '\0' && r < RID_MAX; ++i) {
        char c = ridIn[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z')) {
            rid[r++] = c;
        }
    }
    rid[r] = '\0';

    int idIn   = doc["id"]   | -1;
    int spanIn = doc["span"] | -1;

    if (idIn < 0 || idIn > 255 || !isValidId((uint8_t)idIn)) {
        Console::warn(TAG, "Demande d'historique : id inconnu (" + String(idIn) + ")");
        publishError(rid, 0, 0, "badreq");
        return;
    }

    if (spanIn < 1 || spanIn > (int)SPAN_HOURS_MAX) {
        Console::warn(TAG, "Demande d'historique : profondeur hors bornes ("
                          + String(spanIn) + " h)");
        publishError(rid, (uint8_t)idIn, 0, "badreq");
        return;
    }

    // Slot unique. Refuser explicitement plutôt que d'empiler évite qu'un appui
    // répété sur le bouton mette la carte en file d'attente de travail flash,
    // et donne à l'interface une réponse au lieu d'un silence.
    ArchiveClearStatus clearStatus = archiveClearStatus;
    if (clearStatus == ArchiveClearStatus::Pending ||
        clearStatus == ArchiveClearStatus::Running ||
        state != State::Idle || requestPending) {
        Console::info(TAG, "Demande d'historique refusée : stockage occupé");
        publishError(rid, (uint8_t)idIn, (uint8_t)spanIn, "busy");
        return;
    }

    // Champs posés AVANT le drapeau : handle() ne peut pas lire une demande
    // partiellement écrite.
    requestedId    = (uint8_t)idIn;
    requestedSpanH = (uint8_t)spanIn;
    strncpy(requestedRid, rid, RID_MAX);
    requestedRid[RID_MAX] = '\0';
    requestPending = true;
}

// =============================================================================
// HANDLE — une opération élémentaire par appel. Tâche TaskManager.
// =============================================================================
void HistoryQuery::handle()
{
    // ─── Priorité à une suppression demandée depuis l'interface web ───────
    if (archiveClearPending) {
        archiveClearPending = false;

        if (state != State::Idle || requestPending) {
            Console::warn(TAG, "Scan d'historique annulé pour supprimer les archives");
        }
        resetScan();
        requestPending = false;

        if (!DataLogger::prepareArchiveClear()) {
            archiveClearStatus = ArchiveClearStatus::Failed;
            return;
        }

        archiveClearStatus = ArchiveClearStatus::Running;
        return;
    }

    // Une seule suppression par tick : l'arrosage reste prioritaire même avec
    // une année complète de fichiers à effacer.
    if (archiveClearStatus == ArchiveClearStatus::Running) {
        DataLogger::ArchiveClearStepResult result =
            DataLogger::clearArchivedHistoryStep();

        if (result == DataLogger::ArchiveClearStepResult::Complete) {
            archiveClearStatus = ArchiveClearStatus::Success;
        } else if (result == DataLogger::ArchiveClearStepResult::Error) {
            archiveClearStatus = ArchiveClearStatus::Failed;
        }
        return;
    }

    // ─── Prise en charge d'une nouvelle demande ──────────────────────────
    if (state == State::Idle) {
        if (!requestPending) return;

        scanId    = requestedId;
        scanSpanH = requestedSpanH;
        strncpy(scanRid, requestedRid, RID_MAX);
        scanRid[RID_MAX] = '\0';
        requestPending = false;

        scanNature  = getMeta((DataId)scanId).nature;
        scanStartMs = millis();
        state       = State::Prepare;

        Console::info(TAG, "Historique demandé : id=" + String(scanId)
                          + " (" + String(getMeta((DataId)scanId).label) + ")"
                          + ", profondeur " + String(scanSpanH) + " h");
    }

    // ─── Garde-fou de durée ──────────────────────────────────────────────
    // Au banc, un journal peut peser plusieurs mégaoctets et un scan complet
    // dépasser cette échéance. La réponse part alors avec ce qui a été
    // collecté. Les fichiers étant parcourus du plus récent au plus ancien,
    // ce qui manque est toujours le passé le plus lointain.
    if (state != State::Publish && state != State::Idle &&
        (millis() - scanStartMs) >= HISTORY_SCAN_DEADLINE_MS) {
        Console::warn(TAG, "Scan abandonné sur échéance ("
                          + String(HISTORY_SCAN_DEADLINE_MS / 1000)
                          + " s) — réponse partielle");
        if (scanFile) scanFile.close();
        partial = true;
        state   = State::Publish;
    }

    switch (state) {
        case State::Prepare:  stepPrepare();  break;
        case State::OpenFile: stepOpenFile(); break;
        case State::ReadFile: stepReadFile(); break;
        case State::Publish:  stepPublish();  break;
        case State::Idle:                     break;
    }
}

// =============================================================================
// PREPARE — fenêtre de temps, grille d'agrégation, liste de fichiers, flush.
//
// C'est le tick le plus coûteux du scan : DataLogger::flushNow() peut écrire un
// bloc de 512 octets sur la flash (30 à 80 ms). C'est exactement le coût que
// DataLogger s'impose déjà de lui-même à chaque seuil de 14 records atteint,
// donc rien de nouveau pour le scheduler — mais il a lieu ici une fois par
// requête d'historique.
// =============================================================================
void HistoryQuery::stepPrepare()
{
    TimeVClock t = VirtualClock::read();
    if (!t.VClock_available) {
        Console::warn(TAG, "Historique impossible : horloge indisponible");
        publishError(scanRid, scanId, scanSpanH, "noclock");
        resetScan();
        return;
    }

    windowTo   = (uint32_t)t.timestamp;
    windowFrom = windowTo - (uint32_t)scanSpanH * 3600UL;

    stepSeconds = (scanSpanH <= FINE_SPAN_MAX_H) ? STEP_FINE_S : STEP_COARSE_S;

    uint32_t slots = ((uint32_t)scanSpanH * 3600UL + stepSeconds - 1) / stepSeconds;
    bucketCount = (uint16_t)((slots > BUCKET_MAX) ? BUCKET_MAX : slots);

    memset(buckets, 0, sizeof(buckets));
    entryCount = 0;
    truncated  = false;
    partial    = false;
    dedupValid = false;

    // Un jour de marge de chaque côté. La rotation ayant lieu à 00h45, un
    // fichier nommé d'après une date couvre à cheval sur deux journées
    // civiles ; le filtrage réel se fait de toute façon sur l'horodatage de
    // chaque record, cette marge ne sert qu'à ne manquer aucun fichier.
    uint32_t days = ((uint32_t)scanSpanH + 23UL) / 24UL;
    fileCount = (uint8_t)(days + 2);
    fileIdx   = 0;

    // Vider le tampon d'écriture de DataLogger AVANT d'ouvrir le moindre
    // lecteur : un handle littlefs est une photographie figée à l'instant de
    // l'ouverture, les records restés en RAM y seraient donc invisibles.
    DataLogger::flushNow();

    state = State::OpenFile;
}

// =============================================================================
// OPEN FILE — une tentative d'ouverture par tick.
//
// Isolée du reste parce que c'est l'opération la plus coûteuse de littlefs
// (parcours de la chaîne CTZ), là où une lecture séquentielle sur un fichier
// déjà ouvert est bon marché.
// =============================================================================
void HistoryQuery::stepOpenFile()
{
    if (fileIdx >= fileCount) {
        state = State::Publish;
        return;
    }

    char path[40];
    buildDayPath((time_t)windowTo - (time_t)fileIdx * 86400L, path, sizeof(path));

    scanFile = LittleFS.open(path, FILE_READ);
    if (!scanFile) {
        // Journée absente : rétention, système arrêté, ou première mise en
        // service. Ce n'est pas une erreur.
        fileIdx++;
        return;
    }

    lineLen      = 0;
    lineOverflow = false;
    dedupValid   = false;
    state        = State::ReadFile;
}

// =============================================================================
// READ FILE — un bloc lu et analysé par tick.
// =============================================================================
void HistoryQuery::stepReadFile()
{
    if (!scanFile) {
        finishFile();
        state = State::OpenFile;
        return;
    }

    size_t n = scanFile.read((uint8_t*)chunk, SCAN_CHUNK_BYTES);
    if (n == 0) {
        // Fin de fichier. Un éventuel reste sans saut de ligne final est
        // abandonné : une ligne tronquée par une écriture en cours ne serait
        // pas exploitable, et finishFile() remet le tampon à zéro.
        finishFile();
        state = State::OpenFile;
        return;
    }

    for (size_t i = 0; i < n; ++i) {
        char c = chunk[i];

        if (c == '\n') {
            if (!lineOverflow && lineLen > 0) {
                line[lineLen] = '\0';
                parseLine(line, lineLen);
            }
            lineLen      = 0;
            lineOverflow = false;
            continue;
        }

        if (c == '\r') continue;

        if (lineLen < CSV_LINE_MAX - 1) {
            line[lineLen++] = c;
        } else {
            // Ligne aberrante : ignorée jusqu'au prochain saut de ligne.
            lineOverflow = true;
        }
    }
}

// =============================================================================
// FIN DE FICHIER — sortie anticipée éventuelle.
// =============================================================================
void HistoryQuery::finishFile()
{
    if (scanFile) scanFile.close();

    dedupValid   = false;
    lineLen      = 0;
    lineOverflow = false;

    // Les fichiers sont parcourus du plus récent au plus ancien et un fichier
    // journalier est chronologique : si le tableau de liste est plein après un
    // fichier entier, aucune entrée d'un fichier plus ancien ne pourra plus y
    // entrer. Inutile de lire la suite.
    if (scanNature != DataNature::metrique && entryCount >= ENTRY_MAX) {
        truncated = true;
        fileIdx   = fileCount;
        return;
    }

    fileIdx++;
}

// =============================================================================
// PARSE — une ligne CSV 7 champs :
// timestamp,VClock_available,VClock_reliable,type,id,valueType,value
// =============================================================================
void HistoryQuery::parseLine(const char* csv, size_t len)
{
    // Repérage des 6 premières virgules. Le 7e champ peut en contenir lui-même
    // s'il s'agit d'un texte entre guillemets : il est donc pris tel quel
    // jusqu'à la fin de la ligne.
    size_t  pos[6];
    uint8_t found = 0;
    for (size_t i = 0; i < len && found < 6; ++i) {
        if (csv[i] == ',') pos[found++] = i;
    }
    if (found < 6) return;

    // Filtre d'identifiant en premier : c'est le rejet le plus fréquent, un
    // journal mêlant les 39 données du système en une seule séquence.
    size_t idLen = pos[4] - pos[3] - 1;
    if (idLen == 0 || idLen > 3) return;
    if (strtol(csv + pos[3] + 1, nullptr, 10) != (long)scanId) return;

    uint32_t ts = (uint32_t)strtoul(csv, nullptr, 10);
    if (ts < windowFrom || ts > windowTo) return;

    if (pos[5] - pos[4] != 2) return;      // valueType tient sur un caractère
    char vtype = csv[pos[4] + 1];

    const char* valField = csv + pos[5] + 1;
    size_t      valLen   = len - pos[5] - 1;
    if (valLen == 0) return;

    // ─── Texte : liste d'événements, sans déduplication ──────────────────
    // Chaque record texte est un événement distinct (rapport d'erreur horaire,
    // événement SMS, verdict de démarrage). Deux messages identiques successifs
    // sont deux faits, pas une répétition d'échantillonnage.
    if (scanNature == DataNature::texte) {
        if (vtype != '1') return;

        // Inverse d'escapeCSV : retrait des guillemets encadrants et
        // dédoublement des guillemets internes.
        char   text[TEXT_MAX + 1];
        size_t out    = 0;
        size_t i      = 0;
        bool   quoted = (valField[0] == '"');
        if (quoted) i = 1;

        for (; i < valLen && out < TEXT_MAX; ++i) {
            char c = valField[i];
            if (quoted && c == '"') {
                if (i + 1 < valLen && valField[i + 1] == '"') {
                    text[out++] = '"';
                    ++i;
                } else {
                    break;                 // guillemet de fermeture
                }
            } else {
                text[out++] = c;
            }
        }
        text[out] = '\0';

        insertEntry(ts, 0.0f, text);
        return;
    }

    if (vtype != '0') return;
    float v = strtof(valField, nullptr);

    // ─── État : liste de transitions ─────────────────────────────────────
    // Les états sont publiés périodiquement même sans changement. Sans ce
    // filtre, une liste de 7 jours serait faite de milliers de répétitions.
    // Le report n'est pas propagé d'un fichier au suivant : au plus une entrée
    // redondante par frontière de journée, ce qui ne vaut pas la complexité
    // d'un report inversé alors que les fichiers sont lus à l'envers.
    if (scanNature == DataNature::etat) {
        if (dedupValid && lroundf(dedupLast) == lroundf(v)) return;
        dedupValid = true;
        dedupLast  = v;
        insertEntry(ts, v, "");
        return;
    }

    // ─── Métrique : moyenne par tranche ──────────────────────────────────
    uint16_t index = (uint16_t)((ts - windowFrom) / stepSeconds);
    if (index >= bucketCount) return;

    buckets[index].sum += v;
    buckets[index].count++;
}

// =============================================================================
// INSERTION — tableau trié par horodatage décroissant, plafonné à ENTRY_MAX.
// Conserve donc les entrées les plus récentes, et l'émission peut s'arrêter sur
// épuisement du budget en ne perdant que les plus anciennes.
// =============================================================================
void HistoryQuery::insertEntry(uint32_t ts, float num, const char* text)
{
    if (entryCount >= ENTRY_MAX && ts <= entries[ENTRY_MAX - 1].ts) {
        truncated = true;
        return;
    }

    uint8_t pos = 0;
    while (pos < entryCount && entries[pos].ts > ts) pos++;

    if (entryCount >= ENTRY_MAX) {
        entryCount--;              // la plus ancienne sort
        truncated = true;
    }

    for (uint8_t i = entryCount; i > pos; --i) {
        entries[i] = entries[i - 1];
    }

    entries[pos].ts  = ts;
    entries[pos].num = num;
    strncpy(entries[pos].text, text, TEXT_MAX);
    entries[pos].text[TEXT_MAX] = '\0';
    entryCount++;
}

// =============================================================================
// PUBLISH — construction du payload JSON et émission.
//
// Le tampon de lecture est réemployé comme tampon de payload : les deux usages
// ne se chevauchent jamais, le scan étant terminé quand on arrive ici.
//
// Format des points, uniforme pour les trois natures : [dt, valeur] où dt est
// un décalage en secondes depuis "from". Uniforme parce qu'un format par nature
// multiplierait les chemins de rendu côté interface, et relatif parce que six
// chiffres suffisent là où un horodatage absolu en demande dix.
//   metrique : dt = début de tranche, valeur = moyenne de la tranche
//   etat     : dt = instant de la transition, valeur = état entier
//   texte    : dt = instant du record, valeur = chaîne
// =============================================================================
void HistoryQuery::stepPublish()
{
    if (scanFile) scanFile.close();

    int w = snprintf(chunk, SCAN_CHUNK_BYTES,
                     "{\"rid\":\"%s\",\"id\":%u,\"span\":%u,\"nature\":\"%s\","
                     "\"from\":%lu,\"to\":%lu,\"step\":%lu,\"pts\":[",
                     scanRid,
                     (unsigned)scanId,
                     (unsigned)scanSpanH,
                     natureName(scanNature),
                     (unsigned long)windowFrom,
                     (unsigned long)windowTo,
                     (unsigned long)(scanNature == DataNature::metrique
                                     ? stepSeconds : 0UL));
    if (w <= 0) {
        Console::error(TAG, "Échec construction du payload d'historique");
        resetScan();
        return;
    }
    size_t len = (size_t)w;

    // Réserve pour la fermeture du JSON (drapeaux partial et truncated).
    const size_t TAIL = 48;
    bool   first  = true;
    size_t points = 0;

    if (scanNature == DataNature::metrique) {
        for (uint16_t b = 0; b < bucketCount; ++b) {
            if (buckets[b].count == 0) continue;

            char item[40];
            int n = snprintf(item, sizeof(item), "%s[%lu,%.2f]",
                             first ? "" : ",",
                             (unsigned long)((uint32_t)b * stepSeconds),
                             (double)(buckets[b].sum / (float)buckets[b].count));
            if (n <= 0 || (size_t)n >= sizeof(item)) break;
            if (len + (size_t)n + TAIL >= SCAN_CHUNK_BYTES) {
                truncated = true;
                break;
            }

            memcpy(chunk + len, item, (size_t)n);
            len += (size_t)n;
            first = false;
            points++;
        }
    } else {
        for (uint8_t e = 0; e < entryCount; ++e) {
            char item[TEXT_MAX * 2 + 48];
            int  n;

            if (scanNature == DataNature::etat) {
                n = snprintf(item, sizeof(item), "%s[%lu,%ld]",
                             first ? "" : ",",
                             (unsigned long)(entries[e].ts - windowFrom),
                             (long)lroundf(entries[e].num));
            } else {
                String esc = jsonEscape(entries[e].text);
                n = snprintf(item, sizeof(item), "%s[%lu,\"%s\"]",
                             first ? "" : ",",
                             (unsigned long)(entries[e].ts - windowFrom),
                             esc.c_str());
            }

            if (n <= 0 || (size_t)n >= sizeof(item)) break;
            if (len + (size_t)n + TAIL >= SCAN_CHUNK_BYTES) {
                truncated = true;
                break;
            }

            memcpy(chunk + len, item, (size_t)n);
            len += (size_t)n;
            first = false;
            points++;
        }
    }

    int tailLen = snprintf(chunk + len, SCAN_CHUNK_BYTES - len,
                           "],\"partial\":%s,\"truncated\":%s}",
                           partial   ? "true" : "false",
                           truncated ? "true" : "false");
    if (tailLen <= 0) {
        Console::error(TAG, "Échec fermeture du payload d'historique");
        resetScan();
        return;
    }
    len += (size_t)tailLen;

    Console::info(TAG, "Historique id=" + String(scanId)
                      + " span=" + String(scanSpanH) + "h — "
                      + String(points) + " points, " + String(len) + " octets"
                      + (partial   ? ", partiel"  : "")
                      + (truncated ? ", tronqué" : ""));

    MqttManager::publishHistory(chunk, len);

    resetScan();
}

// =============================================================================
// Chemin du fichier journal de la journée contenant un instant donné.
// Format aligné sur DataLogger::buildFilePath. Le décalage de l'heure de
// rotation n'est pas reproduit ici : stepPrepare ajoute une journée de marge
// de chaque côté, et le filtrage se fait sur l'horodatage des records.
// =============================================================================
void HistoryQuery::buildDayPath(time_t utc, char* out, size_t outSize)
{
    struct tm local;
    localtime_r(&utc, &local);

    snprintf(out, outSize, "/log_%04d-%02d-%02d.csv",
             local.tm_year + 1900,
             local.tm_mon + 1,
             local.tm_mday);
}

// =============================================================================
// Réponse d'erreur. Payload court, sans retain comme toutes les réponses de ce
// canal : un historique retenu serait redélivré périmé à chaque reconnexion.
// =============================================================================
void HistoryQuery::publishError(const char* rid, uint8_t id, uint8_t spanH,
                                const char* reason)
{
    char buf[128];
    int  n = snprintf(buf, sizeof(buf),
                      "{\"rid\":\"%s\",\"id\":%u,\"span\":%u,\"error\":\"%s\"}",
                      rid ? rid : "",
                      (unsigned)id,
                      (unsigned)spanH,
                      reason);
    if (n <= 0) return;

    MqttManager::publishHistory(buf, (size_t)n);
}
