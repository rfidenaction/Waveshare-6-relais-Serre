// Core/DataBus.h
// Bus asynchrone de distribution des données.
//
// Point d'entrée UNIQUE de toute donnée produite dans le système.
//
// Contrat producteur :
//   Le producteur construit un BusItem avec ses champs métier
//   (type, id, valueKind, valueFloat/valueText) puis appelle
//   DataBus::publish(item). UNE SEULE méthode publique.
//
// À chaque publication, DataBus :
//   1. Valide le BusItem contre META (id, type, nature, bornes)
//   2. Horodate via VirtualClock::read() (une seule fois)
//   3. Distribue simultanément et de manière non-bloquante vers :
//      - mqttQueue  (FreeRTOS, drainée par MqttManager)
//      - logQueue   (FreeRTOS, drainée par DataLogger)
//      - WebServer::updateLastData() (portMUX, accès direct)
//      - Si commande : routage via RELAYS[] → handler du manager
//
// Si la validation échoue, le BusItem est rejeté (Console::warn, pas distribué).
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

    // Point d'entrée UNIQUE de toute donnée dans le système.
    //
    // Le producteur remplit les champs métier du BusItem :
    //   type, id, valueKind, valueFloat/valueText
    // Les champs horloge (timestamp, VClock_available, VClock_reliable) sont
    // remplis ICI via VirtualClock::read(), puis le BusItem est distribué
    // vers mqttQueue, logQueue, WebServer et, si commande, routé via RELAYS[].
    static void publish(BusItem& item);

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

    // Vérifie la cohérence du BusItem avec META. Rejet + Console::warn si invalide.
    static bool validate(const BusItem& item);

    // Distribue un BusItem déjà rempli vers toutes les destinations.
    static void distribute(const BusItem& item);

    // Route une commande via RELAYS[] vers le handler du manager propriétaire.
    static bool routeCommand(DataId cmdId, uint32_t durationMs);
};
