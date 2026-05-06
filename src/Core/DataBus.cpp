// Core/DataBus.cpp
// Bus asynchrone de distribution — voir DataBus.h
//
// Flux de données :
//   publish()
//     → VirtualClock::read() (horodatage unique)
//     → distribute() :
//         xQueueSend(mqttQueue)      → MqttManager drain
//         xQueueSend(logQueue)       → DataLogger drain (drop si plein)
//         WebServer::updateLastData() → portMUX, accès direct
//     → si commande : routeCommand() via RELAYS[]

#include "Core/DataBus.h"
#include "Core/VirtualClock.h"
#include "Config/IO-Config.h"
#include "Web/WebServer.h"
#include "Utils/Console.h"

static const char* TAG = "DataBus";

// ─── Variables statiques ─────────────────────────────────────────────────────
QueueHandle_t DataBus::mqttQueue = nullptr;
QueueHandle_t DataBus::logQueue  = nullptr;

// ─── init() ──────────────────────────────────────────────────────────────────
void DataBus::init()
{
    if (mqttQueue == nullptr) {
        mqttQueue = xQueueCreate(MQTT_QUEUE_CAPACITY, sizeof(BusItem));
        if (mqttQueue == nullptr) {
            Console::error(TAG, "Échec création mqttQueue");
        }
    }

    if (logQueue == nullptr) {
        logQueue = xQueueCreate(LOG_QUEUE_CAPACITY, sizeof(BusItem));
        if (logQueue == nullptr) {
            Console::error(TAG, "Échec création logQueue");
        }
    }

    Console::info(TAG, "DataBus initialisé (mqttQueue="
                  + String(MQTT_QUEUE_CAPACITY) + ", logQueue="
                  + String(LOG_QUEUE_CAPACITY) + ")");
}

// ─── distribute() ────────────────────────────────────────────────────────────
// Envoie un BusItem vers les trois destinations. Non-bloquant.
void DataBus::distribute(const BusItem& item)
{
    // MQTT queue — éviction FIFO si pleine (on perd l'ancien)
    if (mqttQueue != nullptr) {
        if (xQueueSend(mqttQueue, &item, 0) != pdTRUE) {
            BusItem evicted;
            (void)xQueueReceive(mqttQueue, &evicted, 0);
            if (xQueueSend(mqttQueue, &item, 0) != pdTRUE) {
                Console::warn(TAG, "mqttQueue : impossible d'empiler id="
                              + String((uint8_t)item.id));
            }
        }
    }

    // Log queue — drop du NOUVEAU si pleine (préserve l'historique ancien)
    if (logQueue != nullptr) {
        if (xQueueSend(logQueue, &item, 0) != pdTRUE) {
            Console::warn(TAG, "logQueue pleine — item id="
                          + String((uint8_t)item.id) + " perdu (nouveau)");
        }
    }

    // lastDataForWeb — mise à jour directe, protégée par portMUX dans WebServer
    WebServer::updateLastData(item);
}

// ─── validate() ──────────────────────────────────────────────────────────────
// Vérifie la cohérence du BusItem avec META avant distribution.
// Retourne true si valide, false + Console::warn si rejeté.
bool DataBus::validate(const BusItem& item)
{
    if (!isValidId((uint8_t)item.id)) {
        Console::warn(TAG, "Rejet : id=" + String((uint8_t)item.id)
                      + " inconnu de META");
        return false;
    }

    const DataMeta& meta = getMeta(item.id);

    bool isCommand = (item.type == DataType::CommandManual ||
                      item.type == DataType::CommandAuto);

    if (isCommand) {
        if (meta.type != DataType::CommandGeneric) {
            Console::warn(TAG, "Rejet : id=" + String((uint8_t)item.id)
                          + " n'est pas une commande dans META");
            return false;
        }
    } else {
        if (item.type != meta.type) {
            Console::warn(TAG, "Rejet : id=" + String((uint8_t)item.id)
                          + " type=" + String((uint8_t)item.type)
                          + " incohérent avec META (attendu "
                          + String((uint8_t)meta.type) + ")");
            return false;
        }
    }

    if (meta.nature == DataNature::texte) {
        if (item.valueKind != 1) {
            Console::warn(TAG, "Rejet : id=" + String((uint8_t)item.id)
                          + " nature=texte mais valueKind=0");
            return false;
        }
    } else {
        if (item.valueKind != 0) {
            Console::warn(TAG, "Rejet : id=" + String((uint8_t)item.id)
                          + " nature=metrique|etat mais valueKind=1");
            return false;
        }
    }

    if (item.valueKind == 0) {
        if (meta.nature == DataNature::metrique) {
            if (item.valueFloat < meta.min || item.valueFloat > meta.max) {
                Console::warn(TAG, "Rejet : id=" + String((uint8_t)item.id)
                              + " valeur=" + String(item.valueFloat, 3)
                              + " hors bornes [" + String(meta.min, 1)
                              + ", " + String(meta.max, 1) + "]");
                return false;
            }
        }
        if (meta.nature == DataNature::etat && meta.stateLabelCount > 0) {
            int stateIdx = (int)item.valueFloat;
            if (item.valueFloat != (float)stateIdx ||
                stateIdx < 0 || stateIdx >= meta.stateLabelCount) {
                Console::warn(TAG, "Rejet : id=" + String((uint8_t)item.id)
                              + " état=" + String(item.valueFloat, 0)
                              + " hors bornes [0, "
                              + String(meta.stateLabelCount - 1) + "]");
                return false;
            }
        }
    }

    return true;
}

// ─── publish() ───────────────────────────────────────────────────────────────
// Point d'entrée UNIQUE. Le producteur a rempli les champs métier
// (type, id, valueKind, valueFloat/valueText). DataBus valide contre META,
// horodate, distribue et, si commande, route via RELAYS[].
void DataBus::publish(BusItem& item)
{
    if (!validate(item)) return;

    TimeVClock t = VirtualClock::read();
    item.timestamp        = static_cast<uint32_t>(t.timestamp);
    item.VClock_available = t.VClock_available;
    item.VClock_reliable  = t.VClock_reliable;

    distribute(item);

    if (item.type == DataType::CommandManual ||
        item.type == DataType::CommandAuto) {
        uint32_t durationMs = (uint32_t)(item.valueFloat * 1000.0f);
        if (!routeCommand(item.id, durationMs)) {
            Console::warn(TAG, "Commande non routée : cmdId="
                          + String((uint8_t)item.id)
                          + " (absente de RELAYS[] ou manager non prêt)");
        }
    }
}

// ─── parseCommand() ──────────────────────────────────────────────────────────
// Fonction PURE. Parse un CSV 7 champs et remplit un BusItem. Aucun effet de bord.
// Les bornes min/max sont validées via META (source de vérité unique).
// Les champs timestamp/VClock ne sont PAS remplis (voir publish).
CommandParseResult DataBus::parseCommand(
    const char* csv, size_t len, BusItem& out)
{
    char buf[64];
    if (len == 0 || len >= sizeof(buf)) {
        return CommandParseResult::BadFormat;
    }
    memcpy(buf, csv, len);
    buf[len] = '\0';

    const char* comma[6];
    int nCommas = 0;
    for (char* p = buf; *p; p++) {
        if (*p == ',') {
            if (nCommas >= 6) return CommandParseResult::BadFormat;
            comma[nCommas++] = p;
        }
    }
    if (nCommas != 6) return CommandParseResult::BadFormat;

    char* f[7];
    f[0] = buf;
    for (int i = 0; i < 6; i++) {
        *const_cast<char*>(comma[i]) = '\0';
        f[i + 1] = const_cast<char*>(comma[i]) + 1;
    }

    auto isEmptyOrZero = [](const char* s) -> bool {
        if (*s == '\0') return true;
        if (*s == '0' && *(s + 1) == '\0') return true;
        return false;
    };
    if (!isEmptyOrZero(f[0]) || !isEmptyOrZero(f[1]) || !isEmptyOrZero(f[2])) {
        return CommandParseResult::TimestampSet;
    }

    char* end = nullptr;
    long typeVal = strtol(f[3], &end, 10);
    if (end == f[3] || *end != '\0') return CommandParseResult::InvalidType;
    if (typeVal != (long)DataType::CommandManual &&
        typeVal != (long)DataType::CommandAuto) {
        return CommandParseResult::InvalidType;
    }

    end = nullptr;
    long idVal = strtol(f[4], &end, 10);
    if (end == f[4] || *end != '\0' || idVal < 0 || idVal > 255) {
        return CommandParseResult::UnknownId;
    }
    if (!isValidId((uint8_t)idVal)) {
        return CommandParseResult::UnknownId;
    }
    DataId cmdId = (DataId)idVal;
    const DataMeta& meta = getMeta(cmdId);
    if (meta.type != DataType::CommandGeneric) {
        return CommandParseResult::NotACommand;
    }

    if (f[5][0] != '0' || f[5][1] != '\0') {
        return CommandParseResult::BadValueType;
    }

    end = nullptr;
    float value = strtof(f[6], &end);
    if (end == f[6] || *end != '\0' || value < meta.min || value > meta.max) {
        return CommandParseResult::BadValue;
    }

    out.id            = cmdId;
    out.type          = (DataType)typeVal;
    out.valueKind     = 0;
    out.valueFloat    = value;
    out.valueText[0]  = '\0';
    return CommandParseResult::OK;
}

// ─── routeCommand() ──────────────────────────────────────────────────────────
// Parcourt RELAYS[] et invoque le handler du manager propriétaire.
bool DataBus::routeCommand(DataId cmdId, uint32_t durationMs)
{
    for (size_t i = 0; i < RELAYS_COUNT; i++) {
        if (RELAYS[i].command == cmdId) {
            return RELAYS[i].enqueue(RELAYS[i].entity, durationMs);
        }
    }
    return false;
}

// ─── tryPopMqtt() ────────────────────────────────────────────────────────────
bool DataBus::tryPopMqtt(BusItem& out)
{
    if (mqttQueue == nullptr) return false;
    return (xQueueReceive(mqttQueue, &out, 0) == pdTRUE);
}

// ─── tryPopLog() ─────────────────────────────────────────────────────────────
bool DataBus::tryPopLog(BusItem& out)
{
    if (logQueue == nullptr) return false;
    return (xQueueReceive(logQueue, &out, 0) == pdTRUE);
}
