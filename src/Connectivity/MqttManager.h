// src/Connectivity/MqttManager.h
#pragma once

#include <Arduino.h>
#include "Config/MetaDataModel.h"

struct BusItem;  // forward declaration (défini dans Core/DataBus.h)

// Client MQTT non-bloquant (esp_mqtt natif ESP-IDF 4.4.4).
// esp-mqtt gère sa propre tâche FreeRTOS ; MqttManager se limite à un slot
// « in-flight » (pour retry sur échec d'enqueue) et un watchdog zombie pour
// la robustesse face aux coupures et aux brokers muets (pas de PUBACK).
//
// Toute publication susceptible de partir du thread TaskManager passe par
// esp_mqtt_client_enqueue : le message est déposé dans l'outbox et c'est la
// tâche esp_mqtt qui écrit sur la socket. Ce thread pilote les vannes, il ne
// doit jamais attendre le réseau. Seules les émissions exclusivement issues de
// l'event handler esp_mqtt — pong, préférences d'affichage — utilisent
// esp_mqtt_client_publish, qui écrit sur la socket dans le thread appelant.
//
// La backpressure amont est portée par DataBus::mqttQueue (queue FreeRTOS,
// capacité 30, éviction FIFO) : c'est elle qui absorbe les bursts métier
// et les coupures WiFi.
//
// Intégration :
//  - init() appelé dans loopInit() après WiFiManager
//  - handle() en tâche TaskManager période 200 ms
//  - handle() pop 1 item de DataBus::mqttQueue, format CSV + enqueue esp_mqtt
//  - setOnPublishSuccess(cb) : callback externe sur PUBACK (BridgeManager)

class MqttManager {
public:
    // ─── Paramètres ajustables (rythme métier) ───────────────────────────
    //
    // Mécanisme global
    // ────────────────
    // - handle() pop 1 item de DataBus::mqttQueue dès que mqttConnected
    //   == true, le formate en CSV et l'enqueue dans esp_mqtt. Chaque
    //   enqueue incrémente messagesEnqueued.
    // - Si l'enqueue échoue (esp_mqtt saturée / déconnectée), le record
    //   formaté reste dans inFlightPayload et sera réémis au tour suivant
    //   (pas de perte sur erreur transitoire).
    // - À chaque PUBACK reçu, les compteurs ET le décompte watchdogSeconds
    //   sont remis à zéro → le broker est prouvé réactif.
    // - Si le broker devient muet (aucun PUBACK), messagesEnqueued s'accumule
    //   pendant que watchdogSeconds décompte. Quand les DEUX conditions
    //   "gap ≥ seuil" et "watchdog expiré" sont réunies simultanément, on
    //   force un disconnect ; esp-mqtt reconnecte seule ~10 s plus tard.
    //
    // Rôle de chaque paramètre
    // ────────────────────────
    // WATCHDOG_GAP_THRESHOLD (enqueues sans PUBACK) : seuil à partir duquel
    //   le broker est suspecté muet. Plus bas = alerte plus sensible, sans
    //   conséquence tant que le watchdog n'expire pas. Règle : proche de la
    //   taille d'un burst typique (détection en 1 ou 2 bursts silencieux
    //   consécutifs selon la taille réelle des bursts).
    //
    // WATCHDOG_SECONDS (secondes) : fenêtre de patience après que le seuil
    //   de gap a été franchi, avant de forcer un disconnect. Si un seul
    //   PUBACK revient pendant cette fenêtre, tout est reset et aucune
    //   action n'est prise. Règle d'or : LÉGÈREMENT SUPÉRIEUR à l'intervalle
    //   de bursts (ex. 65 min pour un cycle ~60 min). Le burst suivant sert
    //   ainsi de « sonde naturelle » : s'il produit un PUBACK, le broker
    //   est prouvé sain et on évite un disconnect inutile. Ne décompte QUE
    //   quand mqttConnected == true (une coupure WiFi ne consomme pas la
    //   fenêtre).
    //
    static constexpr uint32_t WATCHDOG_GAP_THRESHOLD = 7;      // enqueues sans PUBACK
    static constexpr uint32_t WATCHDOG_SECONDS       = 3900;   // 65 min

    // RETAINED_REFRESH_MS : période de republication des quatre éléments à
    //   durée de vie longue (schéma, table NEO, programmation horaire, règles
    //   conditionnelles). Le plan HiveMQ Cloud Serverless expire les messages
    //   retenus au bout de 3 jours ; sur une installation stable, où plus rien
    //   n'est publié spontanément, un téléphone neuf ne trouverait donc plus
    //   ni schéma, ni table NEO, ni programmations. Valeur très en deçà des
    //   72 h afin de tolérer deux échéances manquées consécutives.
    static constexpr uint32_t RETAINED_REFRESH_MS = 24UL * 3600UL * 1000UL;  // 24 h

    // RETAINED_REFRESH_SPACING_MS : intervalle entre les quatre émissions de
    //   l'échéance ci-dessus. Groupées, elles chargent l'outbox esp_mqtt et la
    //   radio en une fois, et les deux premières — schéma et NEO, seules émises
    //   par handle() lui-même — prennent le verrou d'API esp_mqtt dans le même
    //   tour de boucle (les deux programmations partent des handle() de
    //   GardenerManager et ConditionalWatering, donc de tours voisins). Étalées,
    //   un tour n'en émet jamais plus d'une ; il peut toujours y ajouter l'item
    //   de DataBus::mqttQueue du drain courant, soit deux prises du verrou au
    //   pire. La valeur n'a aucune contrainte haute : le rafraîchissement
    //   dispose de 72 h avant l'expiration côté broker.
    static constexpr uint32_t RETAINED_REFRESH_SPACING_MS = 60UL * 1000UL;   // 1 min

    static void init();
    static void ensureMqttStarted();
    static bool isMqttConnected();
    static void handle();
    static void setOnPublishSuccess(void (*callback)());

    // Publication de l'état Gardener (retain sur serre/gardener/ToUser).
    // Passe-plat : reçoit le JSON prêt de GardenerManager.
    static void publishGardenerWateringState(const char* payload, size_t len);

    // Publication de l'état de l'arrosage conditionnel
    // (retain sur serre/conditional/ToUser).
    // Passe-plat : reçoit le JSON prêt de ConditionalWatering.
    static void publishConditionalState(const char* payload, size_t len);

    // Publication d'une réponse d'historique (serre/history/ToUser).
    // Passe-plat : reçoit le JSON prêt de HistoryQuery.
    //
    // SANS retain : une réponse d'historique répond à une question posée à un
    // instant donné, ce n'est pas un état du système. Retenue, elle serait
    // redélivrée périmée à chaque reconnexion d'un téléphone et afficherait un
    // graphique obsolète.
    static void publishHistory(const char* payload, size_t len);

    // ─── Familles (noms utilisateur pour les 6 vannes/capteurs) ──────────
    static constexpr uint8_t FAMILY_COUNT    = 6;
    static constexpr uint8_t FAMILY_NAME_MAX = 24;

    // Charge /families.json au boot. Si absent, initialise "Famille".
    static void loadFamilyNames();

private:
    static void* mqttClient;
    static volatile bool mqttConnected;
    static bool mqttStarted;
    static bool schemaPublished;

    // Horodatage millis() de la dernière publication du schéma, réarmé par
    // publishSchema() quelle que soit son origine (connexion, renommage de
    // famille, échéance périodique). Sert d'échéance à RETAINED_REFRESH_MS.
    static uint32_t lastSchemaPublishMs;

    // Séquenceur du rafraîchissement périodique. L'échéance des 24 h n'émet
    // que le schéma, puis arme ce compteur : 1 = NEO, 2 = programmation
    // horaire, 3 = règles conditionnelles, 0 = aucune séquence en cours.
    // retainedRefreshStepMs porte l'horodatage millis() de la dernière étape
    // émise et est réarmé à chaque étape, si bien qu'une coupure du lien en
    // cours de séquence ne provoque aucune rafale de rattrapage au retour.
    static uint8_t  retainedRefreshStep;
    static uint32_t retainedRefreshStepMs;

    static void mqttEventHandler(void* handlerArgs, const char* base, int32_t eventId, void* eventData);
    // serre/cmd (CSV 7 champs) : DataBus::parseCommand → DataBus::publish
    static void dispatchCommand(void* eventData);
    // Reponse immediate au ping de l'interface, sur MQTT_PING_TOPIC_TO_USER.
    // Emise depuis l'event handler esp_mqtt, sans retain : un pong retenu
    // serait redelivre a chaque abonnement et afficherait la carte en ligne
    // alors qu'elle serait eteinte.
    static void publishPong();
    static void publishSchema();
    static String buildSchemaJson();

    // Table NEO, publiée retenue sur son propre topic, aux mêmes moments que
    // le schéma. Le contenu est figé au démarrage, mais la republication
    // périodique reste nécessaire : le broker expire les messages retenus.
    static void publishNeo();
    static String buildNeoJson();
    static String formatCsvPayload(const BusItem& item);

    static void (*_onPublishSuccess)();

    // Slot « in-flight » : un BusItem déjà formaté en CSV, en attente de
    // succès d'enqueue esp_mqtt. Si l'enqueue courant échoue, on retente
    // le même payload au tour suivant — aucun item n'est perdu sur
    // erreur transitoire. La backpressure globale (bursts, coupures WiFi)
    // est portée par DataBus::mqttQueue en amont.
    static char    inFlightPayload[200];
    static uint8_t inFlightId;
    static bool    inFlightBusy;

    // Watchdog zombie MQTT.
    // Tout PUBACK remet les trois compteurs à zéro. handle() décrémente
    // watchdogSeconds tant que mqttConnected. Si gap >= seuil ET
    // watchdogSeconds == 0 → esp_mqtt_client_disconnect().
    // volatile : cross-thread TaskManager vs esp-mqtt. uint32_t atomique ESP32.
    static volatile uint32_t messagesEnqueued;
    static volatile uint32_t messagesPublished;
    static volatile uint32_t watchdogSeconds;
    static uint32_t forcedDisconnectCount;  // diag cumul depuis boot

    // ─── Signal MqttKo (Waveshare → LilyGo via BridgeManager) ────────────
    // mqttKoDownSinceMs : horodatage millis() du debut de la deconnexion
    //   courante. Arme (≠ 0) au premier MQTT_EVENT_DISCONNECTED qui suit
    //   une periode connectee, desarme (= 0) a chaque MQTT_EVENT_CONNECTED.
    // mqttKoLastSentMs : horodatage millis() du dernier MqttKo envoye
    //   dans l'episode de deconnexion courant. Reset a chaque reconnexion.
    // Ces deux champs ne sont ecrits QUE depuis handle() / event handler
    // (thread esp_mqtt) ; lus uniquement dans handle() pour la decision
    // d'envoi. Pas de cross-thread sensible (ecritures aux transitions
    // connected/disconnected, lecture periodique 1 Hz).
    static uint32_t mqttKoDownSinceMs;
    static uint32_t mqttKoLastSentMs;
    static uint32_t mqttKoSentCount;    // diag cumul depuis boot

    // ─── Familles ────────────────────────────────────────────────────────
    static char familyNames[FAMILY_COUNT][FAMILY_NAME_MAX + 1];
    static bool saveFamilyNames();
    static void handleFamilyRename(const char* data, int len);

    // ─── Préférences d'affichage de l'interface ──────────────────────────
    // Réglages qui n'existent que pour l'œil : quel capteur occupe la 3ᵉ case
    // des cartes Famille, quel nom libre porte chaque boîtier du chapitre
    // Capteurs. La carte n'en tire aucune décision ; elle sert de mémoire
    // commune pour que deux téléphones affichent la même serre.
    //
    // Le contenu est stocké TEL QUEL, sans être interprété. Les clés de
    // boîtier (« sol:1 », « air:14 ») sont dérivées des libellés META par
    // l'interface : les redéfinir ici dupliquerait META, ce que ce projet
    // s'interdit. Seule la taille est bornée, et elle l'est en dessous de
    // cfg.buffer_size (1024) : un message entrant plus long arriverait
    // fragmenté en plusieurs MQTT_EVENT_DATA, cas que le routage par topic
    // de mqttEventHandler() ne sait pas traiter.
    //
    // Ni migration ni numéro de format : /uiprefs.json est un fichier neuf,
    // introduit avec l'interface V17.5. Tant qu'il n'existe pas, la carte
    // publie un objet vide et l'interface applique ses valeurs par défaut.
    static constexpr size_t UIPREFS_MAX_LEN = 900;

    static char   uiPrefs[UIPREFS_MAX_LEN + 1];
    static size_t uiPrefsLen;

    static void loadUiPrefs();
    static bool saveUiPrefs();
    static void handleUiPrefs(const char* data, int len);
    static void publishUiPrefs();
};