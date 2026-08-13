// Utils/StatusReport.h
// Deux verdicts issus de la console série, publiés comme des données normales.
//
//   DataId::Boot  ("Démarrage") — publié UNE SEULE FOIS à BOOT_VERDICT_DELAY_MS.
//                 Contient la première anomalie survenue depuis le démarrage,
//                 ou "OK". Ne se rejoue jamais.
//
//   DataId::Error ("Erreur")    — publié à l'instant du verdict de démarrage,
//                 puis toutes les STATUS_REPORT_PERIOD_MS. Contient le nombre
//                 d'ERROR et de WARN de la fenêtre écoulée, plus le premier
//                 message de chaque catégorie, ou "OK". Les compteurs sont
//                 remis à zéro après chaque publication.
//
// Sources :
//   - automatique : Console::log() appelle onConsoleError() / onConsoleWarn()
//                   avant son filtre de niveau, donc indépendamment du réglage
//                   de verbosité
//   - explicite   : note() permet à un module de signaler au verdict de
//                   démarrage une anomalie grave qui n'est pas une ERROR
//                   (pile RTC HS)
//   - différée    : handle() évalue à l'échéance du démarrage les critères qui
//                   ne peuvent être jugés qu'à ce moment (absence de synchro
//                   NTP, système vannes non opérationnel)
//
// Contraintes tenues :
//   - aucune allocation mémoire dans le chemin d'erreur (composition par
//     recopie caractère par caractère dans des buffers statiques)
//   - ni virgule ni guillemet dans les textes produits : ces caractères sont
//     remplacés par une espace à la capture, pour que le CSV reste trivial à
//     relire par n'importe quel consommateur
//   - module purement observateur : aucune action corrective, aucune
//     entrée-sortie, aucun travail supplémentaire quand le système va mal
#pragma once

#include <Arduino.h>

class StatusReport {
public:
    // Anomalie destinée au seul verdict de démarrage. Premier arrivé, seul
    // retenu. N'incrémente aucun compteur.
    // Appelable depuis n'importe quel thread (les erreurs MQTT sont émises
    // depuis la tâche interne d'esp-mqtt).
    static void note(const String& tag, const String& message);

    // Crochets de Console::log(). Comptent la fenêtre courante et retiennent
    // le premier message de leur catégorie. onConsoleError() alimente en plus
    // le verdict de démarrage.
    static void onConsoleError(const String& tag, const String& message);
    static void onConsoleWarn (const String& tag, const String& message);

    // Tâche périodique. Ne fait rien tant que BOOT_VERDICT_DELAY_MS n'est pas
    // écoulé. Publie ensuite le verdict de démarrage puis, dans le même
    // passage, le premier rapport ; ensuite un rapport par période.
    static void handle();

private:
    // Verdict de démarrage : message tronqué à 150 caractères
    // (BusItem::valueText en accepte 199).
    static constexpr size_t MESSAGE_SIZE = 151;

    // Premier message de chaque catégorie dans un rapport : 60 caractères.
    // Deux blocs de 60 plus les compteurs tiennent dans REPORT_SIZE.
    static constexpr size_t FIRST_MSG_SIZE = 61;

    // Texte du rapport périodique. Majorant : 24 caractères de compteurs
    // plus deux blocs de 65 = 154.
    static constexpr size_t REPORT_SIZE = 160;

    static char _message[MESSAGE_SIZE];
    static bool _latched;     // une anomalie est mémorisée dans _message
    static bool _published;   // le verdict de démarrage est parti sur DataBus

    // ─── Fenêtre courante du rapport périodique ──────────────────────────
    static uint16_t _errorCount;
    static uint16_t _warnCount;
    static char     _firstError[FIRST_MSG_SIZE];
    static char     _firstWarn[FIRST_MSG_SIZE];

    // Échéance du prochain rapport. Touchée par handle() uniquement.
    static uint32_t _nextReportMs;

    // Compose le rapport de la fenêtre écoulée, le publie et remet les
    // compteurs à zéro.
    static void publishReport();
};
