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
 *
 * Note : les timings liés à la politique d'alertes SMS (grâce, cooldown,
 *        délai de boot) sont centralisés dans SmsManager.h, en tête de fichier,
 *        avec les flags d'activation correspondants. Un seul endroit pour
 *        tout ce qui concerne les SMS.
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
#define TASKMON_MIN_ACCEPTABLE_PERIOD_MS    1997
#define TASKMON_MAX_ACCEPTABLE_PERIOD_MS    2003

// Note : les réglages du SMS d'alerte TaskMon (activation, grâce, cooldown)
//        sont dans SmsManager.h, section "POLITIQUE D'ALERTES SMS".

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
// ValveManager — Démarrage différé du pilote des électrovannes
// =============================================================================
/*
 * Délai avant que ValveManager accepte les commandes d'ouverture (3 min 30 s).
 *
 * Les GPIO relais sont forcés à "fermé" dès setup() par initPinsSafe()
 * (protection matérielle immédiate, indépendante de toute initialisation
 * logicielle). Mais le pilote lui-même n'accepte les commandes openFor()
 * qu'après ce délai.
 *
 * Objectif :
 *  - laisser WiFi, MQTT, WebServer, NTP se stabiliser sans interférence
 *  - éviter que les premiers cycles d'arrosage ne perturbent les inits
 *    (allocations, handshakes TLS, resynchronisations)
 *
 * Positionné juste avant BRIDGE_START_DELAY_MS (4 minutes) pour garantir
 * que les vannes peuvent fonctionner avant que le trafic UDP vers LilyGo
 * ne commence.
 *
 * IMPORTANT : ce délai ne dépend d'aucune condition réseau. L'arrosage
 * fonctionne même si le WiFi n'a jamais été établi.
 */
#define VALVE_START_DELAY_MS           210000UL    // 3 min 30 s

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
 * Période d'appel de DataLogger::handle() (drain intake + réparation UTC +
 * alimentation egress + décision flush SPIFFS).
 *
 * Calée sur 1 s depuis le refactor route unifiée :
 *  - aligne la cadence de drain intake sur celle de MqttManager::handle,
 *    ce qui rend symétriques les délais de publication des mesures, des
 *    états et des commandes (plus d'asymétrie ON/OFF sur les vannes
 *    courtes : ON et OFF tombent sur deux ticks DataLogger distincts et
 *    sont publiés à l'intervalle physique réel, au pas de 1 s près).
 *  - la politique de flush SPIFFS reste pilotée par FLUSH_SIZE (trigger
 *    à 50 records en PENDING) et par la fenêtre horaire. La période de
 *    handle n'influe pas sur la fréquence d'écriture flash, seulement
 *    sur la réactivité d'évaluation des seuils.
 *  - coût CPU par tick hors flush : quelques µs (drain + scan rapide
 *    de PENDING). Négligeable à 1 Hz.
 */
#define DATALOGGER_HANDLE_PERIOD_MS    1000

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