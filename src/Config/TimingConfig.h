// Config/TimingConfig.h
#pragma once
/*
 * TimingConfig
 *
 * Centralisation de TOUS les paramètres temporels SYSTÈME.
 *
 * Règle fondamentale :
 *  - ici : uniquement des timings liés au fonctionnement du moteur
 *  - jamais de timings métier (arrosage, seuils agronomiques, etc.)
 *
 * Objectifs :
 *  - lisibilité long terme
 *  - cohérence globale
 *  - éviter les timings implicites dispersés dans le code
 */

// =============================================================================
// Démarrage système
// =============================================================================
/*
 * Durée de la phase d'initialisation système (INIT).
 *
 * Pendant cette phase :
 *  - les modules matériels sont initialisés
 *  - aucune tâche périodique n'est exécutée
 *  - TaskManager / EventManager / Monitor sont inactifs
 *
 * Objectif :
 *  - laisser le matériel et les bus se stabiliser
 *  - éviter toute mesure ou alerte non significative au boot
 */
#define SYSTEM_INIT_DELAY_MS   2500

// =============================================================================
// EventManager / TaskManager supervision
// =============================================================================
/*
 * Période nominale d'appel d'EventManager par TaskManager.
 * Définit le rythme attendu du cœur du système en régime permanent.
 */
#define EVENT_MANAGER_PERIOD_MS        2000

/*
 * Fenêtre temporelle acceptable autour de la période nominale.
 *
 * Si l'intervalle réel entre deux appels sort de cette plage,
 * TaskManagerMonitor bascule en état WARNING.
 */
#define EVENT_MANAGER_MIN_PERIOD_MS    1500
#define EVENT_MANAGER_MAX_PERIOD_MS    2500

// =============================================================================
// WiFi
// =============================================================================
/*
 * Période d'appel de WiFiManager::handle() (machine d'états non-bloquante).
 */
#define WIFI_HANDLE_PERIOD_MS          250

/*
 * Période d'enregistrement des informations WiFi (RSSI, état).
 * Utilisé pour le suivi long terme, pas pour la réactivité immédiate.
 */
//  #define WIFI_STATUS_LOG_INTERVAL_MS    60000
#define WIFI_STATUS_UPDATE_INTERVAL_MS 30000UL

// =============================================================================
// UTC / NTP
// =============================================================================
/*
 * Période d'appel de ManagerUTC::handle() (machine d'état autonome).
 */
#define UTC_HANDLE_PERIOD_MS           2000

/*
 * Période entre deux tours de la routine NTP post-boot (50 min).
 * À chaque tour, un essai NTP est tenté si nécessaire.
 */
#define NTP_RETRY_PERIOD_MS            3000000UL   // 50 min

/*
 * Nombre de tours entre deux essais NTP quand VClock est déjà synced.
 * 25 tours × 50 min ≈ 20h50.
 */
#define NTP_ROUTINE_TOUR_COUNT         25

// =============================================================================
// DataLogger
// =============================================================================
/*
 * Période d'appel de DataLogger::handle() (flush SPIFFS + réparation UTC).
 */
#define DATALOGGER_HANDLE_PERIOD_MS    30000

// =============================================================================
// SafeReboot — Reboot préventif automatique
// =============================================================================
/*
 * Jour et heure cible du reboot mensuel (heure locale France).
 * Le reboot a lieu le 1er de chaque mois à 12h25 ± 5 min.
 *
 * 12h25 est choisi pour :
 *  - laisser le temps à l'envoi des données de 12h00 (émission horaire)
 *  - que VirtualClock, si elle démarre à son ancre par défaut (12h30),
 *    soit à ~5 minutes de l'heure réelle après reboot
 */
#define SAFE_REBOOT_TARGET_DAY         1
#define SAFE_REBOOT_TARGET_HOUR        12
#define SAFE_REBOOT_TARGET_MINUTE      25

/*
 * Période d'appel de SafeReboot::handle() par TaskManager.
 */
#define SAFE_REBOOT_PERIOD_MS          300000UL    // 5 minutes

/*
 * Fallback si UTC n'est jamais disponible (RTC mort + pas de NTP).
 * Reboot après 45 jours - 5 minutes d'uptime.
 * Valeur en microsecondes (int64_t).
 */
#define SAFE_REBOOT_FALLBACK_US        ((int64_t)(45ULL * 86400ULL - 300ULL) * 1000000LL)

// =============================================================================
// Réservé – extensions futures
// =============================================================================
// Capteurs environnementaux
// #define AIR_SENSOR_UPDATE_INTERVAL_MS      ...
// #define SOIL_SENSOR_UPDATE_INTERVAL_MS     ...

// Stockage / maintenance
// #define FILESYSTEM_MAINTENANCE_INTERVAL_MS ...