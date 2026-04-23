// Storage/DataLogger.cpp
// Portage Waveshare ESP32-S3-Relay-6CH
// Refactoring temps :
//  - push() utilise ManagerUTC::readUTC() pour PENDING et LastDataForWeb
//  - push() utilise VirtualClock::nowVirtual() pour LIVE (axe métier)
//  - handle() réparation via ManagerUTC::readUTC()
//  - tryFlush() sans garde RTCManager — flushe tout record UTC_available
//  - Format CSV 7 champs : timestamp,UTC_available,UTC_reliable,type,id,valueType,value
//  - LastDataForWeb toujours alimenté
//
// Refactoring META (source de vérité unique) :
//  - escapeCSV() et jsonEscape() centralisées comme méthodes publiques
//  - init() utilise findMetaIndex() au lieu de DataId::Count
//  - isValidId() remplace les tests manuels de bornes
//
// Suppression getGraphCsv() :
//  - La route /graphdata est supprimée côté WebServer
//  - Les graphiques sont désormais servis par /logs/download (bundle complet)
//  - Le filtrage par DataId et le sous-échantillonnage sont faits côté client
//    dans PagePrincipale.cpp (même principe que le client MQTT distant)
#include "Storage/DataLogger.h"
#include "Connectivity/ManagerUTC.h"
#include "Core/VirtualClock.h"
#include "Config/TimingConfig.h"
#include "Utils/Console.h"
#include <SPIFFS.h>
#include <time.h>

// Tag pour logs Console
static const char* TAG = "DataLogger";

// -----------------------------------------------------------------------------
// Buffers
// -----------------------------------------------------------------------------
DataRecord DataLogger::live[LIVE_SIZE];
DataRecord DataLogger::pending[PENDING_SIZE];

size_t DataLogger::liveIndex    = 0;

// Pending FIFO circulaire
size_t DataLogger::pendingHead  = 0;   // index du plus ancien
size_t DataLogger::pendingCount = 0;   // nombre d'éléments valides

std::array<LastDataForWeb, META_COUNT> DataLogger::lastDataForWeb{};
std::array<bool,           META_COUNT> DataLogger::lastDataForWebHas{};

// Queue intake unifiée (thread-safe, remplie par submit*() depuis
// n'importe quel thread, drainée par handle() côté TaskManager)
QueueHandle_t DataLogger::intake = nullptr;

// Queue egress unifiée (thread-safe, remplie par applyIntakeItem sur
// TaskManager, drainée par MqttManager et autres subscribers à leur rythme)
QueueHandle_t DataLogger::egress = nullptr;

static unsigned long lastFlushMs = 0;

// -----------------------------------------------------------------------------
// Utilitaires partagés — centralisés ici, déclarés publics dans DataLogger.h
// Utilisés par DataLogger, WebServer, MqttManager
// -----------------------------------------------------------------------------

// Échappe une String pour CSV : ajoute guillemets et double les guillemets internes
String DataLogger::escapeCSV(const String& text)
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

// Libellé canonique d'un DataType.
// Source de vérité unique pour l'affichage des types dans les schémas JSON.
// Couvre aussi les types absents de META (CommandManual, CommandAuto).
const char* DataLogger::typeLabel(DataType t)
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

// Échappement JSON minimal (caractères critiques uniquement)
// Les accents et caractères UTF-8 multi-octets sont valides en JSON sans escape.
String DataLogger::jsonEscape(const char* s)
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

// -----------------------------------------------------------------------------
// Helpers CSV internes — parsing (utilisé uniquement dans ce fichier)
// -----------------------------------------------------------------------------

// Parse une String CSV (entre guillemets) et dé-échappe
// Entrée: "texte" ou "texte ""quoted"""
// Sortie: texte ou texte "quoted"
static String unescapeCSV(const String& text)
{
    String unescaped = "";

    if (text.length() < 2 || text.charAt(0) != '"' || text.charAt(text.length() - 1) != '"') {
        Console::warn(TAG, "CSV String sans guillemets: " + text);
        return text;
    }

    for (size_t i = 1; i < text.length() - 1; i++) {
        char c = text.charAt(i);
        if (c == '"') {
            if (i + 1 < text.length() - 1 && text.charAt(i + 1) == '"') {
                unescaped += '"';
                i++;
            } else {
                Console::warn(TAG, "Guillemet non échappé dans CSV");
                unescaped += c;
            }
        } else {
            unescaped += c;
        }
    }

    return unescaped;
}

// -----------------------------------------------------------------------------
// Initialisation
// -----------------------------------------------------------------------------
void DataLogger::init()
{
    lastFlushMs = millis();

    pendingHead  = 0;
    pendingCount = 0;

    // Queue intake unifiée — créée une seule fois au boot.
    // Tous les submit*() (mesures, états, textes, commandes) déposent ici
    // depuis n'importe quel thread. handle() draine côté TaskManager.
    // Si la création échoue, les submit*() perdront leurs items avec un
    // warning mais ne bloqueront pas.
    if (intake == nullptr) {
        intake = xQueueCreate(INTAKE_CAPACITY, sizeof(IntakeItem));
        if (intake == nullptr) {
            Console::error(TAG, "Échec création queue intake — journalisation désactivée");
        }
    }

    // Queue egress unifiée — créée une seule fois au boot.
    // Alimentée par applyIntakeItem, drainée par tryPopForPublish (côté
    // MqttManager et futurs subscribers). Si la création échoue, l'egress
    // est silencieusement désactivé (le callback legacy reste opérationnel
    // pour l'étape 2 de transition).
    if (egress == nullptr) {
        egress = xQueueCreate(EGRESS_CAPACITY, sizeof(EgressRecord));
        if (egress == nullptr) {
            Console::error(TAG, "Échec création queue egress — publication désactivée");
        }
    }

    // Reconstruction LastDataForWeb depuis la flash
    // LECTURE UNIQUE du fichier CSV : on parcourt toutes les lignes
    // et on garde la dernière valeur rencontrée pour chaque DataId.
    // Format CSV 7 champs : timestamp,UTC_available,UTC_reliable,type,id,valueType,value

    File file = SPIFFS.open("/datalog.csv", FILE_READ);
    if (!file) {
        // Fichier n'existe pas — normal au premier boot
        return;
    }

    // Table temporaire indexée par position dans META (pas par id)
    struct LastSeen {
        bool found = false;
        uint32_t timestamp = 0;
        bool UTC_available = false;
        bool UTC_reliable  = false;
        std::variant<float, String> value;
    };
    LastSeen lastSeen[META_COUNT];

    while (file.available()) {
        String line = file.readStringUntil('\n');
        if (line.length() == 0) continue;

        // Parser la ligne : timestamp,UTC_available,UTC_reliable,type,id,valueType,value
        int c1 = line.indexOf(',');
        int c2 = line.indexOf(',', c1 + 1);
        int c3 = line.indexOf(',', c2 + 1);
        int c4 = line.indexOf(',', c3 + 1);
        int c5 = line.indexOf(',', c4 + 1);
        int c6 = line.indexOf(',', c5 + 1);

        if (c1 == -1 || c2 == -1 || c3 == -1 || c4 == -1 || c5 == -1 || c6 == -1) {
            continue;  // Ligne mal formatée, ignorer
        }

        unsigned long ts     = line.substring(0, c1).toInt();
        uint8_t avail        = line.substring(c1 + 1, c2).toInt();
        uint8_t reliable     = line.substring(c2 + 1, c3).toInt();
        // typeByte (c3→c4) ignoré : META est la source de vérité pour le type
        uint8_t idByte       = line.substring(c4 + 1, c5).toInt();
        uint8_t valueType    = line.substring(c5 + 1, c6).toInt();
        String valueStr      = line.substring(c6 + 1);

        // Recherche de l'index dans META pour cet id
        int metaIdx = findMetaIndex(idByte);
        if (metaIdx < 0) continue;  // Id inconnu dans META, ignorer

        LastSeen& ls = lastSeen[metaIdx];
        ls.found         = true;
        ls.timestamp     = ts;
        ls.UTC_available = (avail != 0);
        ls.UTC_reliable  = (reliable != 0);

        if (valueType == 0) {
            ls.value = valueStr.toFloat();
        } else {
            valueStr.trim();
            ls.value = unescapeCSV(valueStr);
        }
    }

    file.close();

    // Peupler lastDataForWeb depuis la table temporaire (indexé par position META)
    for (size_t m = 0; m < META_COUNT; m++) {
        if (lastSeen[m].found) {
            LastDataForWeb& w = lastDataForWeb[m];
            w.value         = lastSeen[m].value;
            w.timestamp     = lastSeen[m].timestamp;
            w.UTC_available = lastSeen[m].UTC_available;
            w.UTC_reliable  = lastSeen[m].UTC_reliable;
            lastDataForWebHas[m] = true;
        }
    }
}

// -----------------------------------------------------------------------------
// SUBMIT — point d'entrée unifié pour valeurs NUMÉRIQUES (float)
// Thread-safe : enqueue dans l'intake sans toucher aux buffers internes.
// DataType déduit automatiquement de META (source de vérité unique).
// Capture horloge à l'appel (VirtualClock pour LIVE, TimeUTC pour PENDING).
// -----------------------------------------------------------------------------
void DataLogger::submit(DataId id, float value)
{
    TimeUTC t = ManagerUTC::readUTC();

    IntakeItem item;
    item.id            = id;
    item.type          = getMeta(id).type;
    item.valueKind     = 0;
    item.valueFloat    = value;
    item.valueText[0]  = '\0';
    item.vClock        = static_cast<uint32_t>(VirtualClock::nowVirtual());
    item.utcTimestamp  = static_cast<uint32_t>(t.timestamp);
    item.UTC_available = t.UTC_available;
    item.UTC_reliable  = t.UTC_reliable;

    enqueueIntakeItem(item);
}

// -----------------------------------------------------------------------------
// SUBMIT — point d'entrée unifié pour valeurs TEXTUELLES (String)
// Thread-safe : enqueue dans l'intake sans toucher aux buffers internes.
// Texte tronqué à 199 caractères dans la queue (stockage POD contraint par
// memcpy FreeRTOS). Avertissement émis en cas de troncature.
// -----------------------------------------------------------------------------
void DataLogger::submit(DataId id, const String& textValue)
{
    TimeUTC t = ManagerUTC::readUTC();

    IntakeItem item;
    item.id            = id;
    item.type          = getMeta(id).type;
    item.valueKind     = 1;
    item.valueFloat    = 0.0f;

    const size_t maxLen = sizeof(item.valueText) - 1;
    size_t srcLen = textValue.length();
    size_t copyLen = (srcLen > maxLen) ? maxLen : srcLen;
    memcpy(item.valueText, textValue.c_str(), copyLen);
    item.valueText[copyLen] = '\0';
    if (srcLen > maxLen) {
        Console::warn(TAG, "submit(String) : texte tronqué "
                      + String((unsigned)srcLen) + " → "
                      + String((unsigned)maxLen) + " caractères (id="
                      + String((uint8_t)id) + ")");
    }

    item.vClock        = static_cast<uint32_t>(VirtualClock::nowVirtual());
    item.utcTimestamp  = static_cast<uint32_t>(t.timestamp);
    item.UTC_available = t.UTC_available;
    item.UTC_reliable  = t.UTC_reliable;

    enqueueIntakeItem(item);
}

// -----------------------------------------------------------------------------
// PARSE COMMAND — fonction PURE (pas d'effet de bord)
//
// Décode et valide un CSV 7 champs. Les 3 premiers (timestamp, UTC_available,
// UTC_reliable) DOIVENT être vides ou "0" (l'émetteur n'horodate pas ;
// l'horodatage sera posé par traceCommand au plus tôt côté carte). En cas
// d'OK, remplit `out`. Sinon, `out` est indéfini.
//
// Appelable depuis n'importe quel thread — aucune I/O, aucune allocation.
// -----------------------------------------------------------------------------
DataLogger::CommandParseResult DataLogger::parseCommand(
    const char* csv, size_t len, ParsedCommand& out)
{
    // Copie locale null-terminée. 64 octets couvrent largement un CSV de
    // commande (ex. ",,,5,255,0,99999" = 18 caractères).
    char buf[64];
    if (len == 0 || len >= sizeof(buf)) {
        return CommandParseResult::BadFormat;
    }
    memcpy(buf, csv, len);
    buf[len] = '\0';

    // Localise les 6 virgules. Exactement 6 attendues, sinon format invalide.
    const char* comma[6];
    int nCommas = 0;
    for (char* p = buf; *p; p++) {
        if (*p == ',') {
            if (nCommas >= 6) return CommandParseResult::BadFormat;
            comma[nCommas++] = p;
        }
    }
    if (nCommas != 6) return CommandParseResult::BadFormat;

    // Découpe : null-terminaison en place à chaque virgule.
    char* f[7];
    f[0] = buf;
    for (int i = 0; i < 6; i++) {
        *const_cast<char*>(comma[i]) = '\0';
        f[i + 1] = const_cast<char*>(comma[i]) + 1;
    }

    // ─── Champs 0..2 : timestamp / UTC_available / UTC_reliable ─────────
    // Doivent être vides ou exactement "0". Tout autre contenu = rejet :
    // l'émetteur ne doit pas prétendre avoir horodaté.
    auto isEmptyOrZero = [](const char* s) -> bool {
        if (*s == '\0') return true;
        if (*s == '0' && *(s + 1) == '\0') return true;
        return false;
    };
    if (!isEmptyOrZero(f[0]) || !isEmptyOrZero(f[1]) || !isEmptyOrZero(f[2])) {
        return CommandParseResult::TimestampSet;
    }

    // ─── Champ 3 : type ∈ {CommandManual, CommandAuto} ──────────────────
    char* end = nullptr;
    long typeVal = strtol(f[3], &end, 10);
    if (end == f[3] || *end != '\0') return CommandParseResult::InvalidType;
    if (typeVal != (long)DataType::CommandManual &&
        typeVal != (long)DataType::CommandAuto) {
        return CommandParseResult::InvalidType;
    }
    DataType origin = (DataType)typeVal;

    // ─── Champ 4 : id valide et META.type == CommandGeneric ─────────────
    end = nullptr;
    long idVal = strtol(f[4], &end, 10);
    if (end == f[4] || *end != '\0' || idVal < 0 || idVal > 255) {
        return CommandParseResult::UnknownId;
    }
    if (!isValidId((uint8_t)idVal)) {
        return CommandParseResult::UnknownId;
    }
    DataId cmdId = (DataId)idVal;
    if (getMeta(cmdId).type != DataType::CommandGeneric) {
        return CommandParseResult::NotACommand;
    }

    // ─── Champ 5 : valueType == 0 (seules les commandes float sont acceptées) ─
    if (f[5][0] != '0' || f[5][1] != '\0') {
        return CommandParseResult::BadValueType;
    }

    // ─── Champ 6 : value > 0 (durée en secondes) ────────────────────────
    end = nullptr;
    float duration = strtof(f[6], &end);
    if (end == f[6] || *end != '\0' || !(duration > 0.0f)) {
        return CommandParseResult::BadValue;
    }

    out.cmdId      = cmdId;
    out.origin     = origin;
    out.durationMs = (uint32_t)(duration * 1000.0f);
    return CommandParseResult::OK;
}

// -----------------------------------------------------------------------------
// SUBMIT COMMAND — journalisation thread-safe d'une commande déjà parsée
//
// Capture les DEUX horloges au plus tôt (VirtualClock pour LIVE, TimeUTC pour
// PENDING + lastDataForWeb), puis dépose dans l'intake. Le record effectif
// est construit dans drainIntake() côté TaskManager.
//
// Appelable depuis n'importe quel thread (esp_mqtt, AsyncTCP, TaskManager).
// Ne retourne pas d'erreur : un échec (queue non prête ou saturée) est loggé
// en warning mais n'interrompt pas le flux d'exécution de la commande — le
// routage applicatif (CommandRouter::route) reste indépendant.
// -----------------------------------------------------------------------------
void DataLogger::submitCommand(const ParsedCommand& cmd)
{
    TimeUTC t = ManagerUTC::readUTC();

    IntakeItem item;
    item.id            = cmd.cmdId;
    item.type          = cmd.origin;           // CommandManual ou CommandAuto
    item.valueKind     = 0;
    item.valueFloat    = cmd.durationMs / 1000.0f;
    item.valueText[0]  = '\0';
    item.vClock        = static_cast<uint32_t>(VirtualClock::nowVirtual());
    item.utcTimestamp  = static_cast<uint32_t>(t.timestamp);
    item.UTC_available = t.UTC_available;
    item.UTC_reliable  = t.UTC_reliable;

    enqueueIntakeItem(item);
}

// -----------------------------------------------------------------------------
// ENQUEUE INTAKE ITEM — helper privé, non-bloquant, thread-safe
//
// Point d'entrée FreeRTOS unique des trois submit*(). xQueueSend timeout 0 :
// jamais de blocage. En cas de saturation ou d'init manquante, warning et
// retour silencieux — l'item NEUF est perdu pour la trace (comportement
// historique de traceCommand, étendu désormais aux mesures/états).
// -----------------------------------------------------------------------------
void DataLogger::enqueueIntakeItem(const IntakeItem& item)
{
    if (intake == nullptr) {
        Console::warn(TAG, "submit : intake pas prêt — item id="
                      + String((uint8_t)item.id) + " perdu");
        return;
    }

    if (xQueueSend(intake, &item, 0) != pdTRUE) {
        Console::warn(TAG, "submit : intake plein — item id="
                      + String((uint8_t)item.id) + " perdu");
    }
}

// -----------------------------------------------------------------------------
// DRAIN INTAKE — consomme la queue depuis handle() (TaskManager)
// -----------------------------------------------------------------------------
void DataLogger::drainIntake()
{
    if (intake == nullptr) return;

    IntakeItem item;
    while (xQueueReceive(intake, &item, 0) == pdTRUE) {
        applyIntakeItem(item);
    }
}

// -----------------------------------------------------------------------------
// APPLY INTAKE ITEM — assemble LIVE + PENDING + lastDataForWeb
//
// Appelée uniquement depuis drainIntake() → thread TaskManager. C'est le
// seul endroit du code qui écrit dans live[], pending[], lastDataForWeb[]
// (hors init()). Invariant C2 des garanties FreeRTOS.
//
// Le champ `type` provient de l'item (déjà résolu par submit* : META pour
// mesure/état/texte, CommandManual/Auto pour les commandes). META n'est PAS
// reconsulté ici.
// -----------------------------------------------------------------------------
void DataLogger::applyIntakeItem(const IntakeItem& item)
{
    // Valeur reconstruite depuis le POD de la queue (variant pour les records)
    std::variant<float, String> value;
    if (item.valueKind == 0) {
        value = item.valueFloat;
    } else {
        value = String(item.valueText);
    }

    // LIVE — horloge VirtualClock capturée à l'enqueue (axe métier)
    DataRecord liveRec;
    liveRec.type          = item.type;
    liveRec.id            = item.id;
    liveRec.value         = value;
    liveRec.timestamp     = item.vClock;
    liveRec.UTC_available = true;
    liveRec.UTC_reliable  = false;
    addLive(liveRec);

    // PENDING — horloge TimeUTC capturée à l'enqueue. Si UTC_available était
    // false, la logique de réparation de handle() corrigera le timestamp dès
    // que l'UTC sera disponible.
    DataRecord pendRec;
    pendRec.type          = item.type;
    pendRec.id            = item.id;
    pendRec.value         = value;
    pendRec.timestamp     = item.utcTimestamp;
    pendRec.UTC_available = item.UTC_available;
    pendRec.UTC_reliable  = item.UTC_reliable;
    addPending(pendRec);

    // Vue Web — dernière valeur (slot indexé par position META)
    int idx = findMetaIndex((uint8_t)item.id);
    if (idx >= 0) {
        LastDataForWeb& w = lastDataForWeb[idx];
        w.value         = value;
        w.timestamp     = item.utcTimestamp;
        w.UTC_available = item.UTC_available;
        w.UTC_reliable  = item.UTC_reliable;
        lastDataForWebHas[idx] = true;
    }

    // Dépôt dans l'egress — route unifiée de publication.
    // Les subscribers (MqttManager::handle pour l'instant, demain d'autres)
    // drainent à leur rythme via tryPopForPublish.
    enqueueEgress(pendRec);
}

// -----------------------------------------------------------------------------
// ENQUEUE EGRESS — producteur unique (applyIntakeItem sur TaskManager)
//
// Sérialise un DataRecord en EgressRecord POD (variant → valueKind +
// valueFloat/valueText). En cas de saturation, éviction FIFO silencieuse :
// on dépile l'item le plus ancien puis on empile le nouveau. L'absence de
// race est garantie par le fait que l'unique producteur tourne sur
// TaskManager.
// -----------------------------------------------------------------------------
void DataLogger::enqueueEgress(const DataRecord& rec)
{
    if (egress == nullptr) return;

    EgressRecord item;
    item.id            = rec.id;
    item.type          = rec.type;
    item.timestamp     = rec.timestamp;
    item.UTC_available = rec.UTC_available;
    item.UTC_reliable  = rec.UTC_reliable;

    if (std::holds_alternative<float>(rec.value)) {
        item.valueKind    = 0;
        item.valueFloat   = std::get<float>(rec.value);
        item.valueText[0] = '\0';
    } else {
        const String& s = std::get<String>(rec.value);
        item.valueKind  = 1;
        item.valueFloat = 0.0f;
        const size_t maxLen = sizeof(item.valueText) - 1;
        size_t srcLen = s.length();
        size_t copyLen = (srcLen > maxLen) ? maxLen : srcLen;
        memcpy(item.valueText, s.c_str(), copyLen);
        item.valueText[copyLen] = '\0';
    }

    if (xQueueSend(egress, &item, 0) != pdTRUE) {
        // Queue pleine — éviction FIFO : dépile l'item le plus ancien
        // et empile le nouveau. Producteur unique = pas de race.
        EgressRecord evicted;
        (void)xQueueReceive(egress, &evicted, 0);
        Console::warn(TAG, "egress plein — record id="
                      + String((uint8_t)evicted.id) + " évincé (FIFO)");
        if (xQueueSend(egress, &item, 0) != pdTRUE) {
            Console::warn(TAG, "egress : réempiler impossible — id="
                          + String((uint8_t)item.id) + " perdu");
        }
    }
}

// -----------------------------------------------------------------------------
// TRY POP FOR PUBLISH — consommateur (MqttManager, futurs subscribers)
//
// Non-bloquant (timeout 0). Reconstruit un DataRecord avec variant<float,
// String> à partir de l'EgressRecord POD. Appelable depuis n'importe quel
// thread (MqttManager::handle tourne sur TaskManager aujourd'hui mais
// l'API reste thread-safe par construction FreeRTOS).
// -----------------------------------------------------------------------------
bool DataLogger::tryPopForPublish(DataRecord& out)
{
    if (egress == nullptr) return false;

    EgressRecord item;
    if (xQueueReceive(egress, &item, 0) != pdTRUE) return false;

    out.id            = item.id;
    out.type          = item.type;
    out.timestamp     = item.timestamp;
    out.UTC_available = item.UTC_available;
    out.UTC_reliable  = item.UTC_reliable;
    if (item.valueKind == 0) {
        out.value = item.valueFloat;
    } else {
        out.value = String(item.valueText);
    }
    return true;
}

// -----------------------------------------------------------------------------
// LIVE
// -----------------------------------------------------------------------------
void DataLogger::addLive(const DataRecord& r)
{
    live[liveIndex] = r;
    liveIndex = (liveIndex + 1) % LIVE_SIZE;
}

// -----------------------------------------------------------------------------
// PENDING — FIFO circulaire avec perte FIFO
// -----------------------------------------------------------------------------
void DataLogger::addPending(const DataRecord& r)
{
    if (pendingCount == PENDING_SIZE) {
        pendingHead = (pendingHead + 1) % PENDING_SIZE;
        pendingCount--;
    }

    size_t index =
        (pendingHead + pendingCount) % PENDING_SIZE;

    pending[index] = r;
    pendingCount++;
}

// -----------------------------------------------------------------------------
// HANDLE — réparation + flush
// -----------------------------------------------------------------------------
void DataLogger::handle()
{
    // Drain de l'intake EN PREMIER, avant la réparation UTC et le flush.
    // Tous les items (mesures, états, textes, traces de commande empilés
    // par n'importe quel thread) sont ainsi injectés dans PENDING à temps
    // pour bénéficier de la réparation UTC de ce même tick si l'UTC vient
    // d'arriver.
    drainIntake();

    TimeUTC t = ManagerUTC::readUTC();
    if (t.UTC_available) {
        for (size_t i = 0; i < pendingCount; ++i) {
            size_t idx = (pendingHead + i) % PENDING_SIZE;
            if (!pending[idx].UTC_available) {
                int32_t deltaMs = static_cast<int32_t>(millis() - pending[idx].timestamp);
                time_t repaired = t.timestamp - static_cast<time_t>(deltaMs / 1000L);

                if (repaired > 0) {
                    pending[idx].timestamp     = static_cast<uint32_t>(repaired);
                    pending[idx].UTC_available = true;
                    pending[idx].UTC_reliable  = t.UTC_reliable;
                }
            }
        }
    }

    bool flushByCount =
        pendingCount >= FLUSH_SIZE;

    bool flushByTime = false;
    if (pendingCount > 0 && millis() - lastFlushMs >= FLUSH_HOURLY_MIN_INTERVAL_MS) {
        if (t.UTC_available) {
            uint32_t secInHour = static_cast<uint32_t>(t.timestamp) % 3600;
            flushByTime = (secInHour < FLUSH_HOURLY_WINDOW_SEC);
        } else {
            flushByTime = (millis() - lastFlushMs >= FLUSH_TIMEOUT_MS);
        }
    }

    if (flushByCount || flushByTime) {
        tryFlush();
    }
}

// -----------------------------------------------------------------------------
// TRY FLUSH — flushe les records UTC_available contigus depuis la tête
// -----------------------------------------------------------------------------
void DataLogger::tryFlush()
{
    size_t flushable = 0;
    for (size_t i = 0; i < pendingCount; ++i) {
        size_t idx = (pendingHead + i) % PENDING_SIZE;
        if (pending[idx].UTC_available) {
            flushable++;
        } else {
            break;
        }
    }

    if (flushable == 0) return;

    size_t toFlush = min(flushable, FLUSH_SIZE);
    flushToFlash(toFlush);
}

// -----------------------------------------------------------------------------
// FLUSH TO FLASH
// Format CSV : timestamp,UTC_available,UTC_reliable,type,id,valueType,value
// valueType = 0 pour float, 1 pour String
// -----------------------------------------------------------------------------
void DataLogger::flushToFlash(size_t count)
{
    File f = SPIFFS.open("/datalog.csv", FILE_APPEND);
    if (!f) {
        Console::error(TAG, "Cannot open /datalog.csv for writing");
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        size_t idx = (pendingHead + i) % PENDING_SIZE;
        DataRecord& r = pending[idx];

        if (std::holds_alternative<float>(r.value)) {
            float val = std::get<float>(r.value);
            f.printf("%lu,%d,%d,%d,%d,0,%.3f\n",
                     r.timestamp,
                     (int)r.UTC_available,
                     (int)r.UTC_reliable,
                     (int)r.type,
                     (int)r.id,
                     val);
        } else {
            String txt = std::get<String>(r.value);
            String escaped = escapeCSV(txt);
            f.printf("%lu,%d,%d,%d,%d,1,%s\n",
                     r.timestamp,
                     (int)r.UTC_available,
                     (int)r.UTC_reliable,
                     (int)r.type,
                     (int)r.id,
                     escaped.c_str());
        }
    }
    f.close();

    pendingHead =
        (pendingHead + count) % PENDING_SIZE;
    pendingCount -= count;

    lastFlushMs = millis();
}

// -----------------------------------------------------------------------------
// CLEAR HISTORY - Suppression historique et réinitialisation
// -----------------------------------------------------------------------------
void DataLogger::clearHistory()
{
    Console::info(TAG, "Suppression de l'historique...");

    if (SPIFFS.remove("/datalog.csv")) {
        Console::info(TAG, "Fichier /datalog.csv supprimé avec succès");
    } else {
        Console::warn(TAG, "Impossible de supprimer /datalog.csv (peut-être inexistant)");
    }

    pendingHead = 0;
    pendingCount = 0;

    Console::info(TAG, "Buffers réinitialisés. Historique vidé.");
}

// -----------------------------------------------------------------------------
// WEB — dernière valeur RAM
// -----------------------------------------------------------------------------
bool DataLogger::hasLastDataForWeb(DataId id, LastDataForWeb& out)
{
    int idx = findMetaIndex((uint8_t)id);
    if (idx < 0)                   return false;
    if (!lastDataForWebHas[idx])   return false;
    out = lastDataForWeb[idx];
    return true;
}

// -----------------------------------------------------------------------------
// STATISTIQUES FICHIER DE LOGS
// -----------------------------------------------------------------------------
LogFileStats DataLogger::getLogFileStats()
{
    LogFileStats stats;
    stats.exists = false;
    stats.sizeBytes = 0;
    stats.sizeMB = 0.0f;
    stats.percentFull = 0.0f;
    stats.totalMB = 2.0f;

    File file = SPIFFS.open("/datalog.csv", FILE_READ);
    if (!file) {
        return stats;
    }

    stats.exists = true;
    stats.sizeBytes = file.size();
    stats.sizeMB = stats.sizeBytes / (1024.0f * 1024.0f);

    file.close();

    stats.percentFull = (stats.sizeMB / stats.totalMB) * 100.0f;

    Console::debug(TAG, "Stats fichier: " + String(stats.sizeMB, 2)
                   + " MB (" + String(stats.percentFull, 1)
                   + "% de " + String(stats.totalMB, 1) + " MB)");

    return stats;
}

// -----------------------------------------------------------------------------
// FLASH — dernière valeur UTC
// Format CSV : timestamp,UTC_available,UTC_reliable,type,id,valueType,value
// -----------------------------------------------------------------------------
bool DataLogger::getLastUtcRecord(DataId id, DataRecord& out)
{
    File file = SPIFFS.open("/datalog.csv", FILE_READ);
    if (!file) {
        Console::error(TAG, "Cannot open /datalog.csv for reading");
        return false;
    }

    String line;
    bool found = false;
    DataRecord candidate;

    while (file.available()) {
        line = file.readStringUntil('\n');
        if (line.length() == 0) continue;

        int c1 = line.indexOf(',');
        int c2 = line.indexOf(',', c1 + 1);
        int c3 = line.indexOf(',', c2 + 1);
        int c4 = line.indexOf(',', c3 + 1);
        int c5 = line.indexOf(',', c4 + 1);
        int c6 = line.indexOf(',', c5 + 1);

        if (c1 == -1 || c2 == -1 || c3 == -1 || c4 == -1 || c5 == -1 || c6 == -1) {
            Console::warn(TAG, "Ligne CSV mal formatée (virgules manquantes): " + line);
            continue;
        }

        uint8_t idByte = line.substring(c4 + 1, c5).toInt();

        if (idByte == static_cast<uint8_t>(id)) {
            candidate.timestamp     = line.substring(0, c1).toInt();
            candidate.UTC_available = (line.substring(c1 + 1, c2).toInt() != 0);
            candidate.UTC_reliable  = (line.substring(c2 + 1, c3).toInt() != 0);
            // typeByte (c3→c4) ignoré : META est la source de vérité pour le type
            candidate.type          = getMeta(id).type;
            candidate.id            = id;

            uint8_t valueType = line.substring(c5 + 1, c6).toInt();
            String valueStr   = line.substring(c6 + 1);

            if (valueType == 0) {
                candidate.value = valueStr.toFloat();
            } else {
                valueStr.trim();
                candidate.value = unescapeCSV(valueStr);
            }
            found = true;
        }
    }

    file.close();
    if (found) {
        out = candidate;
    }
    return found;
}