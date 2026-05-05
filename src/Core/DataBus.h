// Core/DataBus.h
// Bus asynchrone de distribution des données.
//
// Point d'entrée unique de toute donnée produite dans le système.
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
// BusItem — miroir POD de DataRecord (MetaDataModel.h) pour queues FreeRTOS
//
// Mêmes champs, même ordre que DataRecord. Seule différence :
//   DataRecord::value  = std::variant<float, String>  (non memcpy-safe)
//   BusItem::value*    = valueKind + valueFloat + valueText[200]  (POD)
//
// Si DataRecord évolue, BusItem DOIT être mis à jour en conséquence.
// ═════════════════════════════════════════════════════════════════════════════

struct BusItem {
    uint32_t timestamp;
    bool     VClock_available;
    bool     VClock_reliable;
    DataType type;
    DataId   id;
    uint8_t  valueKind;       // 0 = float (valueFloat), 1 = texte (valueText)
    float    valueFloat;      // Valide si valueKind == 0
    char     valueText[200];  // Valide si valueKind == 1, null-terminé
};

// ═════════════════════════════════════════════════════════════════════════════
// CommandParseResult — diagnostic du parsing CSV de commande
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

    // Publication d'une commande déjà parsée dans un BusItem.
    // Complète l'horodatage, distribue et route vers le manager via RELAYS[].
    static void publishCommand(BusItem& item);

    // Parse un CSV 7 champs de commande et remplit un BusItem.
    // Fonction PURE, aucun effet de bord. Valide les bornes via META.
    static CommandParseResult parseCommand(const char* csv, size_t len,
                                           BusItem& out);

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

    // Route une commande via RELAYS[] vers le handler du manager propriétaire.
    static bool routeCommand(DataId cmdId, uint32_t durationMs);
};
