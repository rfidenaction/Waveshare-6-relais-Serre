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
// EventManager
// =============================================================================
/*
 * Période d'appel d'EventManager par TaskManager.
 * EventManager est un observateur pur des sous-systèmes (WiFi, etc.).
 */
#define EVENT_MANAGER_PERIOD_MS        2000

// =============================================================================
// TaskManagerMonitor — Supervision de la régularité du scheduler
// =============================================================================
/*
 * Période d'exécution de la tâche TaskManagerMonitor::checkSchedulerRegularity().
 *
 * Le monitor est enregistré comme une tâche périodique normale auprès du
 * TaskManager. À chaque exécution, il mesure son propre delta temporel par
 * rapport à l'exécution précédente. Si ce delta sort de la plage acceptable,
 * c'est que le scheduler est ralenti ou bloqué par une autre tâche.
 *
 * Ce module ne dépend d'aucun module métier — c'est sa propre régularité
 * d'exécution qui sert de référence.
 */
#define TASKMON_CHECK_PERIOD_MS        2000

/*
 * Fenêtre temporelle acceptable autour de TASKMON_CHECK_PERIOD_MS.
 *
 * Si le delta réel entre deux exécutions de checkSchedulerRegularity() sort
 * de cette plage, une dérive est signalée (log + remontée).
 *
 * CONTRAINTE DE COHÉRENCE :
 *   TASKMON_CHECK_PERIOD_MS DOIT se trouver à l'intérieur de la plage
 *   [TASKMON_MIN_ACCEPTABLE_PERIOD_MS ; TASKMON_MAX_ACCEPTABLE_PERIOD_MS],
 *   sinon le monitor déclencherait des alertes en permanence par construction.
 */
#define TASKMON_MIN_ACCEPTABLE_PERIOD_MS    1500
#define TASKMON_MAX_ACCEPTABLE_PERIOD_MS    2500

/*
 * Période de grâce après init() avant que le SMS d'alerte puisse être armé.
 *
 * Pendant les premières minutes après le démarrage du monitor, des dérives
 * peuvent apparaître à cause de l'initialisation de certains services
 * (WiFi, MQTT, NTP, etc.). On ne veut pas envoyer de SMS pour ces dérives
 * de boot — elles sont quand même loguées et publiées sur MQTT.
 */
#define TASKMON_SMS_GRACE_MS                180000UL    // 3 minutes

/*
 * Cooldown minimum entre deux SMS d'alerte du monitor.
 *
 * En cas de problème durable, on ne veut pas saturer le destinataire de SMS.
 * L'information reste accessible en temps réel via les publications MQTT.
 */
#define TASKMON_SMS_COOLDOWN_MS             172800000UL // 48 heures

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
// BridgeManager — Communication Waveshare ↔ LilyGo
// =============================================================================
/*
 * Délai avant démarrage de BridgeManager (ouverture socket UDP).
 * Laisse le temps au WiFi STA, MQTT et NTP de se stabiliser
 * avant d'introduire du trafic UDP sur la radio partagée.
 */
#define BRIDGE_START_DELAY_MS           240000UL    // 4 minutes

/*
 * Période d'appel de BridgeManager::handle() par TaskManager.
 * À chaque appel : recvfrom non-bloquant + machine d'états SMS.
 * Temps d'exécution : quelques microsecondes.
 */
#define BRIDGE_HANDLE_PERIOD_MS         500

/*
 * Timeout d'attente de l'ACK après envoi d'un SMS (3 minutes).
 * La LilyGo renvoie ACK quand le modem a réellement envoyé le SMS.
 * Si pas d'ACK après ce délai : retry (1 fois) ou abandon.
 */
#define BRIDGE_SMS_ACK_TIMEOUT_MS       180000UL    // 3 minutes

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

/*
 * Délai minimum entre deux flush horaires (55 min).
 * Empêche de re-flusher plusieurs fois dans la fenêtre de 5 min
 * autour de l'heure pleine.
 */
#define FLUSH_HOURLY_MIN_INTERVAL_MS   3300000UL   // 55 min

/*
 * Fenêtre de calage sur l'heure pleine (5 min = 300s).
 * Le flush horaire se déclenche quand UTC % 3600 < cette valeur.
 */
#define FLUSH_HOURLY_WINDOW_SEC        300

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