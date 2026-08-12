// Utils/BootStatus.h
// Verdict de démarrage — publie une fois DataId::Boot ("Démarrage").
//
// Principe :
//   Toute anomalie survenue pendant le démarrage est mémorisée ici. Seule la
//   PREMIÈRE est retenue ; les suivantes sont ignorées (la console série reste
//   la source complète pour le diagnostic détaillé).
//
// Trois sources alimentent la mémorisation :
//   - automatique : Console::log() appelle note() sur tout Console::error()
//   - explicite   : un module signale une anomalie grave qui n'est pas une
//                   ERROR (pile RTC HS)
//   - différée    : handle() évalue à l'échéance les critères qui ne peuvent
//                   être jugés qu'une fois le démarrage terminé (absence de
//                   synchro NTP, système vannes non opérationnel)
//
// À BOOT_VERDICT_DELAY_MS, handle() publie UNE SEULE FOIS DataId::Boot :
// la cause mémorisée, ou "OK" si rien n'est survenu. Aucune republication.
// L'horodatage est celui de DataBus, comme pour toute donnée du système.
#pragma once

#include <Arduino.h>

class BootStatus {
public:
    // Mémorise une anomalie de démarrage. Premier arrivé, seul retenu.
    // Appelable depuis n'importe quel thread (les erreurs MQTT sont émises
    // depuis la tâche interne d'esp-mqtt).
    static void note(const String& tag, const String& message);

    // Tâche périodique. Ne fait rien tant que BOOT_VERDICT_DELAY_MS n'est pas
    // écoulé, publie le verdict au premier passage après l'échéance, puis
    // ressort immédiatement à chaque appel ultérieur.
    static void handle();

private:
    // Message tronqué à 150 caractères (BusItem::valueText en accepte 199).
    static constexpr size_t MESSAGE_SIZE = 151;

    static char _message[MESSAGE_SIZE];
    static bool _latched;     // une anomalie est mémorisée dans _message
    static bool _published;   // le verdict est parti sur DataBus
};
