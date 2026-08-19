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
// TaskManager — décalage de phase
// =============================================================================
/*
 * TaskManager::handle() exécute dans la MÊME passe de boucle toutes les tâches
 * échues : leurs durées s'additionnent dans cette passe. Or les périodes des
 * quatre tâches du bus RS485 sont harmoniques (30, 75, 150 et 300 s) et toutes
 * comptées depuis le démarrage, si bien qu'elles tombaient ensemble toutes les
 * 300 secondes — quatre transactions Modbus bloquantes enchaînées, que
 * TaskManagerMonitor mesure comme une dérive du scheduler.
 *
 * Le remède est de décaler l'origine de chaque grille d'un multiple de ce pas.
 * Le décalage se conserve : chaque tâche repart de sa propre dernière
 * exécution.
 */
#define TASK_PHASE_STEP_MS             5000UL      // 5 s entre deux tâches

/*
 * Rangs attribués. Deux tâches ne peuvent tomber dans la même passe que si
 * l'écart de leurs décalages est un multiple du PGCD de leurs périodes ; les
 * dix paires ont été vérifiées, aucune ne remplit cette condition.
 *
 *   tension alim  30 s   → 1  (5 s)
 *   sondes sol    75 s   → 2  (10 s)
 *   capteurs air  150 s  → 3  (15 s)
 *   capteur boît. 300 s  → 4  (20 s)
 *   état WiFi     1 h    → 5  (4 min + 25 s)
 *
 * Le rang WiFi ajoute VCLOCK_START_DELAY_MS : la première publication doit
 * arriver après la bascule VClock, sinon l'UI garde un horodatage millis()
 * jusqu'au tick suivant, une heure plus tard. Le +25 s conserve un écart
 * non multiple des PGCD RS485 (paires revérifiées avec 265 s).
 *
 * handle() avance chaque échéance d'un nombre entier de périodes, pas de
 * l'horloge murale : l'origine de la grille survit même si deux tâches dues
 * partent dans la même passe, et un retard (démarrage inclus) se résorbe en
 * un seul appel. Une collision ponctuelle reste possible si la boucle a plus
 * de 5 s de retard ; elle ne recolle plus les origines.
 *
 * La vérification des paires ci-dessus dépend des périodes du moment : changer
 * l'effectif d'une famille de capteurs change sa période, et il faut refaire
 * le calcul. Une coïncidence retrouvée ne coûte qu'une passe plus lourde, elle
 * ne casse rien.
 */
#define SUPPLY_VOLTAGE_PHASE_OFFSET_MS (1 * TASK_PHASE_STEP_MS)
#define SOIL_RS485_PHASE_OFFSET_MS     (2 * TASK_PHASE_STEP_MS)
#define AIR_RS485_PHASE_OFFSET_MS      (3 * TASK_PHASE_STEP_MS)
#define INBOX_RS485_PHASE_OFFSET_MS    (4 * TASK_PHASE_STEP_MS)
#define WIFI_STATUS_PHASE_OFFSET_MS    (VCLOCK_START_DELAY_MS + 5 * TASK_PHASE_STEP_MS)

// =============================================================================
// StatusReport — Verdict de démarrage et rapport périodique
// =============================================================================
/*
 * Instant où le démarrage est déclaré terminé et où le verdict est publié
 * sur DataId::Boot ("Démarrage").
 *
 * Calé juste après la première interrogation d'un capteur air (~315–320 s,
 * une fois le bus RS485 ouvert à 285 s). On n'attend plus le tour complet
 * des capteurs : à 5 min par capteur, cela retarderait le verdict sans
 * le rendre plus significatif.
 */
#define BOOT_VERDICT_DELAY_MS          361000UL

/*
 * Période d'appel de StatusReport::handle() par TaskManager.
 * Une comparaison de millis() par appel, hors des deux échéances.
 */
#define STATUS_TASK_PERIOD_MS          5000

/*
 * Période du rapport publié sur DataId::Error ("Erreur") : nombre d'ERROR et
 * de WARN de la fenêtre écoulée, plus le premier message de chaque catégorie.
 *
 * Le premier rapport part à l'instant du verdict de démarrage et couvre donc
 * la séquence de boot ; les suivants couvrent une heure chacun.
 */
#define STATUS_REPORT_PERIOD_MS        3600000UL   // 1 h

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
 * rapport à l'exécution précédente. Cette valeur est aussi la référence à
 * laquelle tout delta mesuré est comparé.
 *
 * Ce module ne dépend d'aucun module métier — c'est sa propre régularité
 * d'exécution qui sert de référence.
 */
#define TASKMON_CHECK_PERIOD_MS        2000

/*
 * Période de publication de la synthèse sur DataId::TaskMonPeriod : la pire
 * période mesurée sur la fenêtre écoulée, c'est-à-dire l'échantillon dont
 * l'écart à TASKMON_CHECK_PERIOD_MS est le plus grand.
 *
 * La publication est inconditionnelle : une fenêtre sans aucune dérive publie
 * la période nominale. L'interface reste ainsi à jour et l'absence de mise à
 * jour devient elle-même un signal.
 *
 * La première synthèse part à BOOT_VERDICT_DELAY_MS et couvre donc la séquence
 * de démarrage ; les suivantes couvrent une fenêtre chacune. Même cadence et
 * même principe que le rapport périodique de StatusReport.
 */
#define TASKMON_REPORT_PERIOD_MS       3600000UL   // 1 h

// Note : les réglages du SMS d'alerte TaskMon (activation, seuils de dérive,
//        grâce, cooldown) sont dans SmsManager.h, section "POLITIQUE D'ALERTES
//        SMS". Ces seuils ne conditionnent que le SMS, jamais l'affichage.

// =============================================================================
// WiFi
// =============================================================================
/*
 * Période d'appel de WiFiManager::handle() (machine d'états non-bloquante).
 */
#define WIFI_HANDLE_PERIOD_MS          250

/*
 * Période de publication des informations WiFi (état STA, état AP, RSSI).
 * Suivi long terme, pas réactivité immédiate : la reconnexion est l'affaire de
 * WiFiManager, appelé lui toutes les WIFI_HANDLE_PERIOD_MS.
 *
 * Conséquence assumée : l'état publié est un instantané horaire. Une coupure
 * WiFi brève entre deux publications ne laisse pas de trace dans le journal.
 *
 * Contrairement aux modules capteurs, cette tâche ne porte aucune logique de
 * première publication : c'est son décalage de phase qui la lui donne.
 * WIFI_STATUS_PHASE_OFFSET_MS place ce premier tick après VClock (4 min +
 * 25 s après l'enregistrement des tâches). Sans lui, l'état WiFi serait
 * absent du journal pendant l'heure suivant chaque reboot ; trop tôt, l'UI
 * garderait un timestamp millis() jusqu'au tick suivant.
 */
#define WIFI_STATUS_UPDATE_INTERVAL_MS 3600000UL   // 1 h

// =============================================================================
// ValveManager — Démarrage différé du pilote des électrovannes
// =============================================================================
/*
 * Délai avant que ValveManager accepte les commandes d'ouverture (4 min 30 s).
 *
 * Les GPIO relais sont forcés à "fermé" dès setup() par initPinsSafe()
 * (protection matérielle immédiate, indépendante de toute initialisation
 * logicielle). Mais le pilote lui-même n'accepte les commandes openFor()
 * qu'après ce délai.
 *
 * Calé juste après VCLOCK_START_DELAY_MS (4 min) pour garantir que VClock
 * a basculé available avant que les premières commandes ne soient horodatées.
 *
 * IMPORTANT : ce délai ne dépend d'aucune condition réseau. L'arrosage
 * fonctionne même si le WiFi n'a jamais été établi.
 */
#define VALVE_START_DELAY_MS           270000UL    // 4 min 30 s

// =============================================================================
// BridgeManager — Communication Waveshare ↔ LilyGo
// =============================================================================
/*
 * Délai avant démarrage de BridgeManager (ouverture socket UDP).
 * Laisse le temps au WiFi STA, MQTT, NTP et à VClock de se stabiliser
 * avant d'introduire du trafic UDP sur la radio partagée.
 */
#define BRIDGE_START_DELAY_MS           300000UL    // 5 minutes

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
// VirtualClock — horloge système unifiée
// =============================================================================
/*
 * Période d'appel de VirtualClock::handle() par TaskManager.
 * handle() gère la bascule initiale (T+4min si NTP a échoué), la resync
 * RTC périodique et la bascule de _reliable après 24h sans sync.
 */
#define VCLOCK_HANDLE_PERIOD_MS        10000

/*
 * Délai avant que VirtualClock tente RTC comme dernier recours.
 * Pendant ces 4 minutes, NTP est la seule source autorisée à déclencher
 * la bascule `_available`. Passé ce délai, handle() tente une lecture
 * RTC ; si elle échoue, ancrage sur 12h30 arbitraire avec `_reliable=false`.
 */
#define VCLOCK_START_DELAY_MS          240000UL    // 4 min

/*
 * Cadence de resynchronisation RTC en régime permanent (cadence absolue).
 * À chaque déclenchement, handle() tente une lecture RTC validée par OSF ;
 * si succès, l'ancre et `_lastSyncMillis` sont recalés.
 */
#define VCLOCK_RTC_RESYNC_PERIOD_MS    10800000UL  // 3 h

/*
 * Délai sans sync au-delà duquel `_reliable` bascule à false.
 * `_reliable` repasse à true dès qu'une sync (NTP ou RTC) réussit.
 * Ce booléen n'a aucune influence en runtime — il est uniquement lu à
 * l'analyse différée des logs.
 */
#define VCLOCK_RELIABLE_TIMEOUT_MS     86400000UL  // 24 h

// =============================================================================
// NTP
// =============================================================================
/*
 * Période d'appel de NTPManager::handle() (machine d'état autonome).
 */
#define NTP_HANDLE_PERIOD_MS           2000

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
 * Période d'appel de DataLogger::handle() (drain logQueue DataBus +
 * réparation UTC + transfert d'un record vers buffer CSV).
 *
 * handle() déplace UN SEUL record réparé par appel vers le buffer CSV actif.
 * À 3 secondes, le flux est lissé : pas de burst de sérialisation.
 * La plupart des appels sont quasi instantanés (logQueue et PENDING vides).
 *
 * Limite de débit : 1 record toutes les 3 secondes = 20 records/minute.
 * Tout débit soutenable pour la flash est largement soutenable pour PENDING.
 */
#define DATALOGGER_HANDLE_PERIOD_MS    3000

/*
 * Heure de rotation quotidienne du fichier log (heure locale).
 * À cette heure, le fichier courant est fermé et un nouveau fichier
 * est ouvert avec la date du jour (log_YYYY-MM-DD.csv).
 */
#define DATALOGGER_ROTATION_HOUR       00
#define DATALOGGER_ROTATION_MINUTE     45

/*
 * Durée de rétention des fichiers log, en jours (une année civile, y compris
 * bissextile). Les fichiers plus anciens sont supprimés à chaque rotation.
 * Volume typique : 5 à 18 Ko/jour. 8 Mo de partition LittleFS tiennent
 * au moins un an ; 366 jours reste en deçà du remplissage, contrairement
 * à une rétention plus longue qui pourrait saturer la flash avant d'effacer.
 */
#define DATALOGGER_RETENTION_DAYS      366

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
// GardenerManager — Programmateur d'arrosage automatique
// =============================================================================
/*
 * Période d'appel de GardenerManager::handle() par TaskManager.
 * 1 seconde : garantit de ne jamais rater une transition de minute.
 */
#define GARDENER_HANDLE_PERIOD_MS      1000

// =============================================================================
// ConditionalWatering — Arrosage conditionnel sur critère de capteur
// =============================================================================
/*
 * Période d'appel de ConditionalWatering::handle() par TaskManager.
 * Le module n'a pas de cadence propre : il réagit aux mesures poussées par
 * DataBus. Cette période ne fixe donc que la latence entre l'arrivée d'une
 * mesure et la décision, la décision ne pouvant pas être prise dans
 * DataBus::distribute() (publish imbriqué).
 */
#define CONDITIONAL_HANDLE_PERIOD_MS   1000

// =============================================================================
// SupplyVoltage — Tension d'alimentation et détection secteur (RS485)
// =============================================================================
/*
 * Période de lecture matérielle de la carte Waveshare Analog Input 8CH (B).
 * Chaque appel effectue une seule transaction Modbus sur Serial1 (~200 ms max).
 *
 * Cette cadence rapide sert uniquement à la détection de front secteur
 * (alerte SMS). La publication sur DataBus est découplée et suit son propre
 * rythme (voir SUPPLY_VOLTAGE_PUBLISH_PERIOD_MS ci-dessous).
 */
#define SUPPLY_VOLTAGE_HANDLE_PERIOD_MS   30000

/*
 * Période de publication de SupplyVoltage et AcPower sur DataBus (battement).
 *
 * La première publication a lieu dès la première lecture réussie (pour que
 * la donnée soit visible à l'interface dès le boot) et pose le point de
 * départ du battement. Les suivantes sont espacées de cette période, sans
 * jamais être recalées. En supplément, un front secteur (coupure ou retour)
 * déclenche une publication extra, soumise au même cooldown d'une heure que
 * le SMS (SmsManager.h), sans recaler le battement horaire. La mesure à la
 * demande n'est pas concernée.
 */
#define SUPPLY_VOLTAGE_PUBLISH_PERIOD_MS  3600000UL   // 1 h

// =============================================================================
// Capteurs RS485 mesurant une température — cadence de lecture commune
// =============================================================================
/*
 * Cadence de lecture de CHAQUE capteur mesurant une température : sondes de
 * sol, capteurs air, capteur boîtier.
 *
 * C'est un plancher matériel, pas un réglage de confort. Interrogé plus
 * souvent, l'élément de mesure s'échauffe et la température lue dérive. Cette
 * valeur ne doit pas être diminuée.
 *
 * Chaque famille interroge UN capteur par appel, en rotation : la période de
 * sa tâche est donc cette cadence divisée par son effectif. La division est
 * faite à l'enregistrement de la tâche, dans main.cpp, à partir du
 * sensorCount() du module. Ajouter ou retirer un capteur ne touche donc aucun
 * timing — chaque capteur reste lu à cette cadence quel que soit l'effectif.
 *
 * La carte Analog Input 8CH (SupplyVoltage) n'est pas concernée : elle ne
 * mesure aucune température et garde sa cadence rapide de détection secteur.
 */
#define RS485_TEMP_READ_PERIOD_MS      300000UL    // 5 min par capteur

// =============================================================================
// SoilSensorRS485 — Sondes de sol RS485 (Modbus RTU)
// =============================================================================
/*
 * Délai avant la première interrogation des sondes de sol.
 * Laisse le système se stabiliser (WiFi, MQTT, NTP, RTC, Gardener)
 * avant d'introduire du trafic sur le bus RS485.
 */
#define SOIL_RS485_START_DELAY_MS      285000UL    // 4 min 45 s

/*
 * Période de publication de chaque sonde de sol sur DataBus.
 *
 * La lecture, elle, tourne à RS485_TEMP_READ_PERIOD_MS : la sonde est lue bien
 * plus souvent qu'elle n'est publiée. Cette cadence rapide sert l'arrosage
 * conditionnel, qui doit rester réactif.
 *
 * Le battement est propre à chaque sonde et posé à sa première lecture réussie,
 * jamais recalé ensuite. La rotation étale donc naturellement les publications
 * des sondes dans l'heure. En supplément, une mesure qui déclenche un arrosage
 * conditionnel est publiée immédiatement par ConditionalWatering, sans affecter
 * le battement.
 */
#define SOIL_RS485_PUBLISH_PERIOD_MS   3600000UL   // 1 h

// =============================================================================
// AirSensorRS485 — Capteurs air RS485 (Ebyte KTH2-R)
// =============================================================================
/*
 * Délai avant la première interrogation des capteurs air.
 * Identique à SOIL_RS485_START_DELAY_MS : les deux familles de capteurs
 * partagent le même bus RS485 (Serial1), déjà ouvert par SoilSensorRS485::init().
 */
#define AIR_RS485_START_DELAY_MS       285000UL    // 4 min 45 s

/*
 * Période de publication de chaque capteur air sur DataBus.
 * Même principe que SOIL_RS485_PUBLISH_PERIOD_MS : battement par capteur, posé
 * à sa première lecture réussie, la lecture restant cadencée par
 * RS485_TEMP_READ_PERIOD_MS.
 */
#define AIR_RS485_PUBLISH_PERIOD_MS    3600000UL   // 1 h

// =============================================================================
// InboxSensorRS485 — Capteur air boîtier (Ebyte KTH2-R, adresse 15)
// =============================================================================
/*
 * Délai avant la première interrogation du capteur boîtier.
 * Identique aux autres capteurs RS485 : même bus, même prérequis de
 * stabilisation.
 */
#define INBOX_RS485_START_DELAY_MS     285000UL    // 4 min 45 s

/*
 * Période de publication de AirTemperature1 et AirHumidity1 sur DataBus.
 *
 * La première publication a lieu dès la première lecture réussie et pose
 * le point de départ du battement. Les suivantes sont espacées de cette
 * période, sans jamais être recalées. En supplément, toute détection de
 * température excessive déclenche une publication immédiate sans affecter
 * le battement horaire.
 */
#define INBOX_RS485_PUBLISH_PERIOD_MS  3600000UL   // 1 h

// =============================================================================
// OnDemandMeasure — mesure à la demande
// =============================================================================
/*
 * Période de vidage du slot de demande.
 * Ne rythme aucune acquisition : la tâche retourne immédiatement s'il n'y a
 * pas de demande en attente. Cette valeur ne fixe donc qu'une latence
 * maximale entre la réception MQTT et l'exécution de la mesure, négligeable
 * devant l'aller-retour réseau via Cat-M (1 à 3 s).
 *
 * Aucun délai de démarrage ici : chaque module producteur applique déjà le
 * sien (SOIL_RS485_START_DELAY_MS, AIR_RS485_START_DELAY_MS) et refuse une
 * mesure à la demande tant qu'il n'est pas écoulé.
 */
#define ONDEMAND_HANDLE_PERIOD_MS      200

// =============================================================================
// HistoryQuery — historique à la demande (graphiques 24 h et 7 jours)
// =============================================================================
/*
 * Période d'avancement du scan. La tâche retourne immédiatement s'il n'y a rien
 * en cours, et exécute au plus UNE opération élémentaire par appel : ouverture
 * d'un fichier, ou lecture d'un bloc de 4 Ko. Le coût par tick est donc de
 * quelques millisecondes, soit le même profil que MqttManager et OnDemandMeasure
 * déjà à 200 ms — aucun régime nouveau n'est introduit dans le scheduler.
 *
 * Cette valeur fixe le débit de lecture : 4 Ko par 200 ms, soit 20 Ko/s. En
 * production (journal d'une douzaine de kilo-octets par jour) une demande 7 j
 * lit moins de 100 Ko et répond en environ 5 s, une demande 24 h en 1 à 2 s.
 *
 * L'accélérer réduirait la latence perçue mais augmenterait d'autant la charge
 * prise sur le temps du scheduler pendant le scan. L'arrosage passe avant le
 * confort d'affichage : cette période est délibérément conservatrice.
 */
#define HISTORY_HANDLE_PERIOD_MS       200

/*
 * Échéance au-delà de laquelle un scan est abandonné. La réponse part avec ce
 * qui a été collecté et le drapeau "partial" ; l'interface l'affiche.
 *
 * Au débit ci-dessus, une minute couvre environ 1,2 Mo de journal. C'est très
 * au-delà des besoins de production, et volontairement en deçà de ce qu'exige
 * un journal de développement (plusieurs mégaoctets pour 7 jours au rythme de
 * 30 s) : au banc, une demande 7 j renvoie donc les journées les plus récentes,
 * les fichiers étant parcourus du plus récent au plus ancien.
 *
 * Ce garde-fou est aussi le filet de sécurité de dernier recours : quoi qu'il
 * arrive à la machine à états, elle revient au repos au bout de cette durée.
 */
#define HISTORY_SCAN_DEADLINE_MS       60000UL

// =============================================================================
// Réservé – extensions futures
// =============================================================================
// Stockage / maintenance
// #define FILESYSTEM_MAINTENANCE_INTERVAL_MS ...