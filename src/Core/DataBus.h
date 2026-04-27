// Core/DataBus.h
// Bus asynchrone de distribution des données.
//
// Point d'entrée unique de toute donnée produite dans le système.
// Remplace tous les appels à DataLogger::push() / submit() / traceCommand().
//
// À chaque publication, DataBus :
//   1. Horodate via VirtualClock::read() (une seule fois)
//   2. Distribue simultanément et de manière non-bloquante vers :
//      - mqttQueue  (FreeRTOS, drainée par MqttManager)
//      - logQueue   (FreeRTOS, drainée par DataLogger)
//      - WebServer::updateLastData() (portMUX, accès direct)
//      - Si commande : routage via RELAYS[] → handler du manager
//
// Thread-safety : publish() est appelable depuis n'importe quel thread.
// Les queues FreeRTOS et le portMUX garantissent la cohérence.
#pragma once

#include "Config/MetaDataModel.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ═════════════════════════════════════════════════════════════════════════════
// BusItem — struct POD transportée dans les queues FreeRTOS (memcpy-safe)
//
// Pas de std::variant, pas de String, pas de pointeur heap.
// valueKind discrimine float vs texte (buffer fixe, tronqué à 199 chars).
// ═════════════════════════════════════════════════════════════════════════════

struct BusItem {
    DataId   id;
    DataType type;
    uint8_t  valueKind;       // 0 = float (valueFloat), 1 = texte (valueText)
    float    valueFloat;      // Valide si valueKind == 0
    char     valueText[200];  // Valide si valueKind == 1, null-terminé

    uint32_t timestamp;
    bool     VClock_available;
    bool     VClock_reliable;
};

// ═════════════════════════════════════════════════════════════════════════════
// ParsedCommand / CommandParseResult — migrés depuis DataLogger
// ═════════════════════════════════════════════════════════════════════════════

enum class CommandParseResult : uint8_t {
    OK            = 0,
    BadFormat,
    TimestampSet,
    InvalidType,
    UnknownId,
    NotACommand,
    BadValueType,
    BadValue
};

struct ParsedCommand {
    DataId   cmdId;
    DataType origin;      // CommandManual ou CommandAuto
    uint32_t durationMs;
};

// ═════════════════════════════════════════════════════════════════════════════
// DataBus — API statique
// ═════════════════════════════════════════════════════════════════════════════

class DataBus {
public:
    // Création des queues FreeRTOS. Appeler une fois au boot, avant DataLogger::init().
    static void init();

    // Publication d'une valeur float (mesure, état numérique).
    // Horodate + distribue vers mqttQueue, logQueue, WebServer.
    static void publish(DataId id, float value);

    // Publication d'une valeur texte (événement, erreur, SMS).
    // Texte tronqué à 199 caractères dans le BusItem.
    static void publish(DataId id, const String& textValue);

    // Publication d'une commande déjà parsée.
    // Horodate + distribue + route vers le manager via RELAYS[].
    static void publishCommand(const ParsedCommand& cmd);

    // Parse un CSV 7 champs de commande. Fonction PURE, aucun effet de bord.
    static CommandParseResult parseCommand(const char* csv, size_t len,
                                           ParsedCommand& out);

    // Pop un item de la queue MQTT. Non-bloquant. Pour MqttManager.
    static bool tryPopMqtt(BusItem& out);

    // Pop un item de la queue log. Non-bloquant. Pour DataLogger.
    static bool tryPopLog(BusItem& out);

private:
    static constexpr uint8_t MQTT_QUEUE_CAPACITY = 30;
    static constexpr uint8_t LOG_QUEUE_CAPACITY  = 80;

    static QueueHandle_t mqttQueue;
    static QueueHandle_t logQueue;

    // Distribue un BusItem déjà rempli vers toutes les destinations.
    static void distribute(const BusItem& item);

    // Route une commande via RELAYS[] (ex-CommandRouter::route).
    static bool routeCommand(DataId cmdId, uint32_t durationMs);
};
