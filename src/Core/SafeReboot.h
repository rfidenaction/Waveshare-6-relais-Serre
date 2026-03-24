// Core/SafeReboot.h
// Reboot préventif automatique
//
// Principe :
//  - 5 minutes après le boot, calcule la deadline du prochain reboot
//  - Si UTC disponible : prochain 1er du mois à 12h25 locale
//  - Si UTC indisponible : 45 jours - 5 minutes (filet de sécurité millis)
//  - Vérifie toutes les 5 minutes si la deadline est atteinte
//  - Avant reboot : flush PENDING SPIFFS, log, vérification vannes (TODO)
//
// Le délai max est ~31 jours (calendaire) ou ~45 jours (fallback),
// toujours sous la limite de débordement de millis() (49.7 jours).
//
// Utilise esp_timer_get_time() (int64_t, µs) en interne pour éviter
// tout problème de débordement sur la comparaison.
#pragma once

#include <Arduino.h>

class SafeReboot {
public:
    static void init();
    static void handle();   // à appeler toutes les 5 minutes (TaskManager)

private:
    // Deadline en µs (esp_timer_get_time), 0 = pas encore calculé
    static int64_t _rebootDeadline;
};