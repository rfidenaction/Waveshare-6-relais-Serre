// Actuators/ValveCycler.h
// Chenillard de test des 6 relais/vannes
//
// Séquence : ouvre Valve1 pendant 5 s, ferme, attend 2 s,
//            ouvre Valve2 pendant 5 s, ferme, attend 2 s, ...
//            jusqu'à Valve6, puis reboucle indéfiniment.
//
// Cycle complet = 6 × 7 s = 42 secondes.
//
// Démarrage différé : le cycler attend que ValveManager soit ready
// (cf. VALVE_START_DELAY_MS dans TimingConfig.h) avant de commencer
// sa séquence. Pendant la phase d'attente, handle() ne fait rien.
//
// Ne logue rien directement : la journalisation Console + MQTT + DataLogger
// est déjà assurée par ValveManager à chaque changement d'état.
//
// À SUPPRIMER en production (fichier de test comme FakeVoltage).
#pragma once

#include <Arduino.h>

class ValveCycler {
public:
    static void init();
    static void handle();   // Appelé toutes les 1000 ms par TaskManager

private:
    static uint8_t secondsCounter;   // 0..41
};