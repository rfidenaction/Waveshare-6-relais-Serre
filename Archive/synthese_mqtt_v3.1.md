# SYNTHÈSE ARCHITECTURE MQTT — SERRE CONNECTÉE
## Document de référence pour implémentation — Version 3.1

**Date** : 17 mars 2026
**Statut** : Validé — Prêt pour implémentation
**Révision** : v3.1 — correction HTTP bloquant BridgeManager + régulation heartbeat

---

## 1. CONTEXTE DU PROJET

### 1.1 Matériel

Deux cartes ESP32-S3 collaborent pour piloter une serre :

**Waveshare ESP32-S3-Relay-6CH** (carte principale)
- Rôle : acquisition capteurs, commande relais/vannes, serveur web local, publication MQTT
- WiFi : mode AP+STA simultané
  - AP : `192.168.5.1` — accès local direct (téléphone/PC connecté au WiFi de la carte)
  - STA : `192.168.4.10` — connectée au pont WiFi-GSM de la LilyGo
- Framework : Arduino (PlatformIO)
- Architecture logicielle : machine d'états non-bloquante, orchestrée par un TaskManager
- **Règle absolue : le TaskManager ne doit JAMAIS être bloqué**

**LilyGo T-SIM7080G-S3** (pont WiFi-GSM)
- Rôle : pont NAT WiFi → Cat-M (PPPoS) vers internet + service SMS
- WiFi SoftAP : `192.168.4.1`, SSID `Pont_Wifi-GSM_de_la_serre`, canal 1, 1 client max
- Modem : SIM7080G, APN `domotec82.fr`
- PMU : AXP2101 (batterie intégrée, voltage/charge lisibles)
- Framework : ESP-IDF pur (FreeRTOS)
- **Ce firmware fonctionne et ne doit PAS être modifié dans un premier temps**
  (sauf ajout du endpoint GET /modem-status, voir §7)

### 1.2 Architecture réseau

```
Internet (Cat-M)
      |
  [ LilyGo T-SIM7080G-S3 ]
    PPPoS ↔ SIM7080G
    SoftAP 192.168.4.1 (NAT/NAPT actif)
      |
    WiFi STA 192.168.4.10
  [ Waveshare ESP32-S3-Relay-6CH ]
    SoftAP 192.168.5.1 (accès local)
      |
    WiFi
  [ Téléphone / PC ]
```

Le Waveshare accède à internet (MQTT, NTP) de manière **transparente** via le NAT
de la LilyGo. DNS configuré en dur : `8.8.8.8`.

### 1.3 Logiciel Waveshare existant

Architecture modulaire, non-bloquante :

```
src/
├── main.cpp                    // setup() + loop() + orchestration (state machine)
├── Config/                     // Config.h, IO-Config.h, NetworkConfig.h, TimingConfig.h
├── Connectivity/
│   ├── WiFiManager.h/.cpp      // Machine d'états WiFi AP+STA (non-bloquant)
│   ├── ManagerUTC.h/.cpp       // Synchronisation NTP
│   └── SmsManager.h/.cpp       // Envoi SMS via POST HTTP vers LilyGo
├── Core/
│   ├── TaskManager.h/.cpp      // Tâches périodiques (cœur du système)
│   ├── EventManager.h/.cpp     // Observation états + futur moteur de règles
│   └── PowerManager.h/.cpp     // PMU, batterie, low power
├── Sensors/
│   └── DataAcquisition.h/.cpp  // Lecture capteurs (BME, sol, débit, etc.)
├── Storage/
│   └── DataLogger.h/.cpp       // Push données + CSV SPIFFS + graphiques web
├── Utils/
│   └── Console.h/.cpp          // Logging
└── Web/
    ├── WebServer.h/.cpp        // AsyncWebServer port 80
    └── Pages/                  // PagePrincipale, PageLogs, etc.
```

### 1.4 Architecture de contrôle : TaskManager + EventManager

Le système repose sur un double mécanisme :

**TaskManager** — exécution périodique. Chaque module enregistre une tâche
via `addTask(callback, period_ms)`. Les callbacks doivent retourner en < 50ms.
C'est le cœur du temps régulier (mesures, polling, maintenance).

**EventManager** — actuellement un embryon (observation de l'état WiFi,
stockage courant/précédent). Destiné à devenir un **moteur de règles** capable
d'évaluer des prédicats composés et de déclencher des actions immédiates.

Exemples de règles futures :
- Si température < 5°C ET hygrométrie > 70% → arrosage réduit + alerte MQTT
- Si température > 30°C ET hygrométrie < 80% → arrosage pluie immédiat 2 min
- Conditions pouvant inclure l'heure (jour/nuit)

EventManager est un sujet majeur en soi, qui sera développé séparément.
Le MqttManager sera à la fois un **fournisseur d'actions** pour ce moteur
(le moteur pourra déclencher une publication MQTT) et un **consommateur**
(recevoir l'ordre de publier une alerte immédiate).

**Important** : la coordination entre les deux cartes (Waveshare ↔ LilyGo)
n'est PAS de la responsabilité d'EventManager. C'est le rôle de
BridgeManager (voir §5).

### 1.5 Pattern de la loop principale

```
setup() → loopInit() [une fois] → loopRun() [boucle infinie]
loopRun() appelle uniquement TaskManager::handle()
```

**DataLogger** : point central des données. Chaque module fait
`DataLogger::push(DataType, DataId, value)`.
Les DataId ont des métadonnées (label, unité, nature) dans `DataMeta`.

**Serveur web actuel** : rendu côté serveur (HTML généré par C++), rafraîchissement
par `location.reload()` toutes les 30s, graphiques via `fetch('/graphdata')` → CSV.

### 1.6 Tâches FreeRTOS indépendantes du TaskManager

Certains composants nécessitent leur **propre tâche FreeRTOS**, car ils effectuent
des opérations potentiellement bloquantes (réseau, TLS) incompatibles avec
la contrainte < 50ms du TaskManager.

Dans ce projet, trois composants tournent dans des tâches FreeRTOS dédiées :

| Composant          | Tâche FreeRTOS        | Raison                                    |
|--------------------|-----------------------|-------------------------------------------|
| esp_mqtt           | créée par esp_mqtt    | TLS handshake, reconnexion, callbacks      |
| BridgeManager      | `bridge_cache_task`   | HTTP GET vers LilyGo (timeout réseau)      |
| *(futur : autres)* |                       |                                            |

Ces tâches communiquent avec le TaskManager via des **structures partagées
thread-safe** (caches avec flags, queues FreeRTOS). Elles ne touchent
JAMAIS directement la logique métier.

**Règle** : si un composant fait du réseau bloquant (HTTP, TLS connect),
il a sa propre tâche. Le TaskManager ne lit que des caches locaux.

---

## 2. BROKER MQTT — HIVEMQ CLOUD

### 2.1 Paramètres de connexion

| Paramètre           | Valeur |
|----------------------|--------|
| URL WebSocket (web)  | `wss://3db6155980d4483e8b8c3036fd0afd6f.s1.eu.hivemq.cloud:8884/mqtt` |
| URL MQTTS (ESP32)    | `mqtts://3db6155980d4483e8b8c3036fd0afd6f.s1.eu.hivemq.cloud:8883` |
| Hostname broker      | `3db6155980d4483e8b8c3036fd0afd6f.s1.eu.hivemq.cloud` |
| Port TLS natif       | `8883` |
| Port WebSocket TLS   | `8884` |
| Username             | `Graindesable` |
| Password             | `Chaperonrouge64` |
| Protocol             | MQTT v5 |
| TLS                  | Obligatoire (pas de connexion non chiffrée) |

### 2.2 Notes importantes

- HiveMQ Cloud gratuit (plan Serverless)
- TLS requis → le client ESP32 doit utiliser un certificat CA racine
  (ISRG Root X1 ou équivalent selon la chaîne HiveMQ)
- **Client IDs doivent être uniques** : HiveMQ déconnecte un client
  si un autre se connecte avec le même ID

---

## 3. BIBLIOTHÈQUE MQTT CHOISIE

### 3.1 Choix : ESP-IDF `esp_mqtt` natif (Espressif)

**Raison du choix** : c'est la bibliothèque la plus utilisée en production sur ESP32.
Tasmota, ESPHome et des millions d'appareils l'utilisent. Maintenue directement
par Espressif. Battle-tested.

**Caractéristiques clés :**
- Tourne dans sa propre tâche FreeRTOS → **jamais de blocage** du TaskManager
- Supporte MQTT v5, TLS (mbedTLS), reconnexion automatique
- API en C pur avec event handlers
- Disponible dans le framework Arduino ESP32 via les headers ESP-IDF intégrés

**Bibliothèques écartées et pourquoi :**
- `PubSubClient` (~4000 stars) : synchrone, bloquante — incompatible avec TaskManager
- `espMqttClient` (~106 stars) : bon mais communauté limitée, connect() bloquant en TLS
  sauf si on crée une tâche FreeRTOS dédiée
- `ESP32MQTTClient` (~41 stars) : wrapper de esp_mqtt, mais trop peu d'utilisateurs
  pour avoir confiance dans la stabilité
- `AsyncMQTT_ESP32` : basé sur l'ancien AsyncMqttClient, TLS problématique

### 3.2 Utilisation depuis Arduino/PlatformIO

Le client `esp_mqtt` est accessible depuis Arduino car le core ESP32 Arduino
est basé sur ESP-IDF. Les headers sont disponibles :
```cpp
#include "mqtt_client.h"  // esp_mqtt_client_config_t, esp_mqtt_client_init, etc.
```

---

## 4. ARCHITECTURE MQTT

### 4.1 Topics

**Données Waveshare → Broker (publication)**
```
serre/data/{label}          // Un topic par DataId
```
Exemples :
- `serre/data/supply_voltage` → `"24.12"`
- `serre/data/air_temperature_1` → `"23.5"`
- `serre/data/air_humidity_1` → `"67.2"`
- `serre/data/soil_moisture_1` → `"45.0"`
- `serre/data/valve1` → `"1"` ou `"0"`
- `serre/data/wifi_sta_connected` → `"1"` ou `"0"`
- `serre/data/wifi_rssi` → `"-67"`

Les labels utilisés sont ceux de DataMeta (identifiants techniques).
Le mapping vers les labels "en clair" (pour affichage) est défini dans DataLogger.h.

**Données LilyGo → Broker (FUTUR, pas implémenté maintenant)**
```
serre/bridge/{label}        // Réservé pour la LilyGo
```
Exemples futurs :
- `serre/bridge/battery_voltage` → `"3.85"`
- `serre/bridge/battery_percent` → `"72"`
- `serre/bridge/charging` → `"1"`
- `serre/bridge/gsm_rssi` → `"-67"`
- `serre/bridge/ppp_status` → `"up"` ou `"down"`
- `serre/bridge/modem_mode` → `"DATA"` ou `"COMMAND"`

**Commandes Téléphone → Waveshare (subscription)**
```
serre/cmd/{action}          // La Waveshare s'y abonne
```
Exemples :
- `serre/cmd/valve1` → payload `"1"` (ouvrir) ou `"0"` (fermer)
- `serre/cmd/ap_disable` → payload `"1"` (couper AP WiFi)

**Statut de connexion (Last Will and Testament)**
```
serre/status/waveshare      // LWT : "offline" automatique si déconnexion
serre/status/bridge         // Réservé LilyGo future
```
- LWT configuré avec **retain = true** et payload `"offline"`
- À la connexion, publication explicite de `"online"` avec **retain = true**
  sur le même topic
- Le téléphone qui se connecte après coup voit immédiatement le dernier
  état retenu par le broker

### 4.2 QoS

| Type de message    | QoS | Justification |
|--------------------|-----|---------------|
| Données capteurs   | 0   | Perte acceptable, prochaine valeur dans 1h max |
| Commandes vannes   | 1   | Garantie de réception indispensable |
| LWT (status)       | 1   | Retain = true, doit persister |

Note sur QoS 1 : assure "at-least-once", pas "exactly-once". Une commande
de vanne peut arriver deux fois. Ceci est sans conséquence car une vanne
est un état (ouvert/fermé), pas une action incrémentale.

⚠️ **À revalider** si le futur moteur de règles introduit des commandes
temporelles (ex: "ouvrir 2 minutes"). Dans ce cas, une duplication
serait un bug. On adaptera le protocole à ce moment-là.

### 4.3 Client IDs

| Carte      | Client ID         |
|------------|-------------------|
| Waveshare  | `serre-waveshare` |
| LilyGo     | `serre-bridge` (réservé, pas utilisé maintenant) |

### 4.4 Payload

Format : **valeur brute en texte** (float ou entier en ASCII).
Pas de JSON pour les données capteurs — inutilement lourd pour des valeurs simples.
Le téléphone connaît la structure des topics.

---

## 5. BRIDGEMANAGER — COORDINATION WAVESHARE ↔ LILYGO

### 5.1 Rôle et périmètre

BridgeManager est le **point unique** de gestion de la relation entre les
deux cartes. Il encapsule tout ce qui concerne le pont LilyGo.

**Responsabilités :**
- Maintenir le cache d'état du modem (ModemStateCache)
- Gérer le flag SMS (smsBusy)
- Envoyer le heartbeat après un cycle de publication MQTT
- Exposer les données LilyGo (batterie, RSSI GSM) au reste du système

**Ce n'est PAS sa responsabilité :**
- Logique métier (→ futur moteur de règles dans EventManager)
- Gestion WiFi (→ WiFiManager)
- Protocole MQTT (→ MqttManager)
- Envoi SMS (→ SmsManager, qui utilise BridgeManager pour la coordination)

### 5.2 Emplacement

```
src/Connectivity/
├── BridgeManager.h/.cpp    // NOUVEAU — Relation Waveshare ↔ LilyGo
├── WiFiManager.h/.cpp      // Relation Waveshare ↔ réseau WiFi
├── MqttManager.h/.cpp      // NOUVEAU — Relation Waveshare ↔ broker HiveMQ
├── ManagerUTC.h/.cpp       // NTP
└── SmsManager.h/.cpp       // Envoi SMS (utilise BridgeManager)
```

### 5.3 ModemStateCache

Structure maintenue par BridgeManager, mise à jour par sa **tâche FreeRTOS
dédiée** (voir §5.7).

```cpp
struct ModemStateCache {
    bool ppp_up;            // État connexion PPP
    bool data_mode;         // true = DATA (internet OK), false = COMMAND (SMS)
    int rssi_gsm;           // Signal cellulaire (dBm)
    int battery_mv;         // Tension batterie (mV)
    int battery_pct;        // Pourcentage batterie
    bool charging;          // En charge
    uint32_t lastUpdateMs;  // Timestamp dernière mise à jour réussie
    bool valid;             // false si LilyGo injoignable
};
```

**Accès thread-safe** : le cache est écrit par la tâche FreeRTOS dédiée
et lu par le TaskManager (via les accesseurs). La protection se fait par
un spinlock (portMUX_TYPE) — même pattern que `last_activity_ms` dans
le firmware LilyGo. Les lectures et écritures sont des copies de struct
complètes sous spinlock, donc atomiques et ultra-rapides (< 1µs).

### 5.4 Flag SMS

```cpp
// BridgeManager expose :
static void setSmsBusy(bool busy);
static bool isSmsBusy();
```

SmsManager encadre ses envois :
```cpp
BridgeManager::setSmsBusy(true);
// ... POST /sms vers LilyGo ...
// ... attente retour PPP ...
BridgeManager::setSmsBusy(false);
```

MqttManager consulte dans son handle() :
```cpp
if (BridgeManager::isSmsBusy()) → état PAUSED
```

Le flag `smsBusy` est un booléen atomique, pas besoin de spinlock.

### 5.5 Heartbeat

```cpp
// BridgeManager expose :
static void sendHeartbeat();
```

Envoie un `GET /heartbeat` vers `http://192.168.4.1/heartbeat`.
Confirme à la LilyGo que toute la chaîne fonctionne.

**Régulation** : le heartbeat est envoyé **une seule fois par cycle
de publication**, pas après chaque message individuel. Si un cycle
publie 10 DataId, un seul heartbeat est envoyé à la fin du cycle.
Ceci évite le spam HTTP vers la LilyGo.

Le heartbeat est exécuté dans la **tâche FreeRTOS de BridgeManager**
(voir §5.7), car c'est un appel HTTP potentiellement bloquant.

**Mécanisme** : MqttManager lève un flag `heartbeatRequested` après un
cycle de publication réussi. La tâche FreeRTOS de BridgeManager voit
ce flag, envoie le heartbeat, et baisse le flag.

### 5.6 Accesseurs état modem

```cpp
// BridgeManager expose (lecture du cache, non-bloquant) :
static bool isModemReady();     // ppp_up && data_mode && !smsBusy && valid
static bool isModemBusy();      // !data_mode (COMMAND mode) ou smsBusy
static bool isBridgeReachable();// cache.valid (LilyGo joignable)

// Données LilyGo (pour affichage web, DataLogger futur)
static int  getBatteryMv();
static int  getBatteryPercent();
static bool isCharging();
static int  getGsmRssi();
```

Tous ces accesseurs lisent le cache sous spinlock. Temps d'exécution
négligeable (< 1µs). **Aucun appel réseau.** Parfaitement compatible
avec la contrainte < 50ms du TaskManager.

### 5.7 Tâche FreeRTOS dédiée (v3.1 — IMPORTANT)

BridgeManager possède sa **propre tâche FreeRTOS** pour toutes les
opérations réseau vers la LilyGo.

**Pourquoi** : les appels HTTP vers 192.168.4.1 (même en local)
peuvent bloquer pendant le timeout réseau. Mettre ces appels dans
le TaskManager violerait la règle du non-bloquant (< 50ms).

**Ce que fait la tâche** (boucle infinie avec vTaskDelay) :

1. **Mise à jour du cache modem** (toutes les ~10s)
   - `GET /modem-status` vers `http://192.168.4.1/modem-status`
   - Parse JSON, écrit le cache sous spinlock
   - Si timeout ou erreur → `cache.valid = false`

2. **Envoi heartbeat** (quand le flag `heartbeatRequested` est levé)
   - `GET /heartbeat` vers `http://192.168.4.1/heartbeat`
   - Baisse le flag après envoi (réussi ou non)

**Paramètres de la tâche :**
```cpp
xTaskCreatePinnedToCore(
    bridgeCacheTask,        // Fonction
    "bridge_cache",         // Nom
    4096,                   // Stack (HTTP + JSON parsing)
    NULL,                   // Paramètre
    3,                      // Priorité (sous MQTT=5, sous SMS=5)
    &bridgeTaskHandle,      // Handle
    0                       // Core 0 (laisser core 1 au loop Arduino)
);
```

**Communication avec le TaskManager :**
- Écriture cache → spinlock (ultra-rapide)
- Flag heartbeat → booléen atomique
- Aucune queue nécessaire (les données sont simples)

**Conséquence** : dans `main.cpp`, BridgeManager n'a **PAS de tâche
TaskManager** pour le polling. La tâche FreeRTOS gère tout le réseau.
Seuls les accesseurs (§5.6) sont appelés depuis le TaskManager.

### 5.8 Intégration dans main.cpp

Dans `loopInit()` :
```cpp
// Initialisation BridgeManager (après WiFi)
// Crée la tâche FreeRTOS dédiée et initialise le cache
BridgeManager::init();
```

Pas de `TaskManager::addTask()` pour BridgeManager — sa tâche FreeRTOS
est autonome. Le TaskManager ne fait que lire les accesseurs.

---

## 6. MODULE MqttManager (Waveshare)

### 6.1 Emplacement

```
src/Connectivity/
├── MqttManager.h
└── MqttManager.cpp
```

Même pattern que WiFiManager : classe statique, machine d'états non-bloquante.

### 6.2 Machine d'états

```
IDLE
  │
  ▼ (WiFi STA connecté)
CHECK_MODEM
  │ (lit BridgeManager, JAMAIS de HTTP direct)
  │
  ├─ BridgeManager::isModemReady() → CONNECTING
  ├─ BridgeManager::isModemBusy() → WAIT_MODEM (attendre 30s)
  ├─ !BridgeManager::isBridgeReachable() → WAIT_RETRY
  │
  ▼
CONNECTING
  │ (esp_mqtt_client_start, non-bloquant)
  │
  ├─ MQTT_EVENT_CONNECTED → CONNECTED
  │     (publie "online" retain=true sur serre/status/waveshare)
  └─ timeout 15s / erreur → CHECK_MODEM
  │
  ▼
CONNECTED
  │
  │ (publication normale, réception commandes)
  │
  ├─ MQTT_EVENT_DISCONNECTED → CHECK_MODEM
  └─ BridgeManager::isSmsBusy() → PAUSED
  │
  ▼
PAUSED
  │ (SMS en cours, aucune tentative de reconnexion)
  │
  └─ !BridgeManager::isSmsBusy() → CHECK_MODEM
  │
  ▼
WAIT_MODEM
  │ (modem en mode COMMAND, on patiente)
  │
  └─ après 30s → CHECK_MODEM
  │
  ▼
WAIT_RETRY
  │ (backoff progressif avec jitter : 15s → 30s → 60s → 120s plafond)
  │ (jitter = ±20% pour éviter reconnexions synchronisées)
  │
  └─ délai écoulé → CHECK_MODEM
```

### 6.3 Queue de commandes entrantes (thread safety)

Les callbacks `esp_mqtt` s'exécutent dans la tâche FreeRTOS du client MQTT,
**PAS** dans le contexte du TaskManager. Accéder directement à des données
partagées depuis ce callback créerait des data races.

**Solution : queue FreeRTOS thread-safe**

```
esp_mqtt callback (tâche esp_mqtt)
    │
    ▼
xQueueSend(mqttIncomingQueue, &message)
    │
    ▼
MqttManager::handle() (tâche TaskManager)
    │ xQueueReceive(mqttIncomingQueue, &message)
    ▼
Action selon topic (commande vanne, etc.)
```

La queue est créée dans `MqttManager::init()`.
Le callback ne fait QUE poster dans la queue.
Le dépilage se fait dans `MqttManager::handle()`, dans le contexte
sûr du TaskManager.

### 6.4 Intégration dans main.cpp

Dans `loopInit()`, après BridgeManager :
```cpp
// Initialisation MQTT
MqttManager::init();

// Tâche machine d'états MQTT (polling état + dépilage commandes)
TaskManager::addTask(
    []() { MqttManager::handle(); },
    MQTT_HANDLE_PERIOD_MS       // 1000ms
);

// Tâche publication périodique capteurs
TaskManager::addTask(
    []() { MqttManager::publishSensors(); },
    MQTT_PUBLISH_SENSORS_PERIOD_MS  // 3600000ms = 1h
);

// Tâche publication périodique système
TaskManager::addTask(
    []() { MqttManager::publishSystem(); },
    MQTT_PUBLISH_SYSTEM_PERIOD_MS   // 300000ms = 5min
);
```

### 6.5 Stratégie de publication (hybride)

**Publication périodique** pour les données régulières :
- `publishSensors()` — capteurs toutes les heures
- `publishSystem()` — WiFi status, uptime toutes les 5 minutes
- Lit les dernières valeurs via `DataLogger::hasLastDataForWeb()`
- **Après chaque cycle**, lève le flag `heartbeatRequested` de BridgeManager
  → un seul heartbeat par cycle, pas par message

**Publication immédiate** pour les événements critiques :
- Changement d'état d'une vanne
- Erreurs système
- Reboot / boot

À terme, le futur moteur de règles (EventManager) pourra déclencher
des publications MQTT immédiates en appelant `MqttManager::publishNow()`.
S'il n'est pas connecté, l'événement est perdu (acceptable — le
prochain cycle périodique publiera l'état courant).

### 6.6 Configuration (NetworkConfig.h)

Nouveaux defines :
```cpp
// MQTT Broker
#define MQTT_BROKER_URI     "mqtts://3db6155980d4483e8b8c3036fd0afd6f.s1.eu.hivemq.cloud:8883"
#define MQTT_USERNAME       "Graindesable"
#define MQTT_PASSWORD       "Chaperonrouge64"
#define MQTT_CLIENT_ID      "serre-waveshare"
#define MQTT_LWT_TOPIC      "serre/status/waveshare"
```

### 6.7 Configuration (TimingConfig.h)

Nouveaux defines :
```cpp
// MqttManager
#define MQTT_HANDLE_PERIOD_MS           1000    // Fréquence machine d'états
#define MQTT_CONNECT_TIMEOUT_MS         15000   // Timeout état CONNECTING
#define MQTT_PUBLISH_SENSORS_PERIOD_MS  3600000 // Publication capteurs (1h)
#define MQTT_PUBLISH_SYSTEM_PERIOD_MS   300000  // Publication WiFi status (5min)
#define MQTT_RECONNECT_BASE_MS          15000   // Backoff initial
#define MQTT_RECONNECT_MAX_MS           120000  // Backoff plafond (2 min)
#define MQTT_RECONNECT_JITTER_PCT       20      // Jitter ±20% sur le backoff
#define MQTT_KEEPALIVE_S                90      // Keepalive MQTT (Cat-M)

// BridgeManager
#define BRIDGE_CACHE_UPDATE_PERIOD_MS   10000   // Intervalle polling /modem-status (10s)
#define BRIDGE_HTTP_TIMEOUT_MS          3000    // Timeout HTTP vers LilyGo
#define BRIDGE_MODEM_WAIT_MS            30000   // Attente si modem en COMMAND
#define BRIDGE_TASK_STACK               4096    // Stack tâche FreeRTOS
#define BRIDGE_TASK_PRIORITY            3       // Priorité (sous MQTT/SMS)
#define BRIDGE_TASK_CORE                0       // Core 0 (core 1 = loop Arduino)
```

---

## 7. ENDPOINT À AJOUTER SUR LA LILYGO

### 7.1 `GET /modem-status`

Seule modification de la LilyGo.

Réponse JSON :
```json
{
  "ppp": "up",
  "mode": "DATA",
  "rssi_gsm": -67,
  "battery_mv": 3850,
  "battery_pct": 72,
  "charging": true
}
```

Champs :
- `ppp` : `"up"` ou `"down"` — état de la connexion PPP
- `mode` : `"DATA"` ou `"COMMAND"` — mode actuel du modem
- `rssi_gsm` : niveau signal cellulaire (dBm)
- `battery_mv` : tension batterie (millivolts, via pmu.getBattVoltage())
- `battery_pct` : pourcentage batterie (via pmu.getBatteryPercent())
- `charging` : état de charge (via pmu.isCharging())

**Risque minimal** : route HTTP en lecture seule, aucun effet de bord,
~15 lignes de code dans le serveur HTTP existant de la LilyGo.

---

## 8. SCÉNARIOS DE COORDINATION

### 8.1 La Waveshare veut publier en MQTT

1. MqttManager appelle `BridgeManager::isModemReady()` (lecture cache, < 1µs)
2. Si prêt → publie normalement
3. Après le cycle complet → lève `heartbeatRequested`
4. La tâche FreeRTOS de BridgeManager envoie le heartbeat

### 8.2 La Waveshare veut envoyer un SMS

1. SmsManager appelle `BridgeManager::setSmsBusy(true)`
2. MqttManager voit `isSmsBusy()` dans son handle() → passe en PAUSED
3. SmsManager fait `POST /sms` vers la LilyGo
4. Attente retour PPP (~10-30s)
5. SmsManager appelle `BridgeManager::setSmsBusy(false)`
6. MqttManager sort de PAUSED → CHECK_MODEM → reconnexion

### 8.3 La LilyGo envoie un SMS de son initiative (boot, alerte)

1. La LilyGo bascule en COMMAND, envoie le SMS, revient en DATA
2. Le MqttManager perd sa connexion MQTT
3. Il consulte BridgeManager qui lit son cache → `data_mode=false`
4. MqttManager → WAIT_MODEM
5. La tâche FreeRTOS de BridgeManager met à jour le cache quand la
   LilyGo revient en DATA (~10s de délai max)
6. MqttManager → CHECK_MODEM → reconnexion

### 8.4 LilyGo injoignable (cache invalide)

1. La tâche FreeRTOS de BridgeManager échoue → `cache.valid = false`
2. MqttManager voit `!isBridgeReachable()` → WAIT_RETRY avec backoff + jitter
3. Continue de réessayer indéfiniment

---

## 9. APPLICATION TÉLÉPHONE (PWA)

### 9.1 Approche

Progressive Web App (PWA) — page web qui se connecte au broker HiveMQ
via MQTT over WebSocket (`mqtt.js`). Installable sur Android comme une app.

### 9.2 Fonctionnement

- Se connecte à `wss://...hivemq.cloud:8884/mqtt` avec les mêmes credentials
- S'abonne à `serre/data/#` pour recevoir toutes les données capteurs
- S'abonne à `serre/bridge/#` pour les données du pont (futur)
- S'abonne à `serre/status/#` pour savoir si les cartes sont en ligne
  (grâce au retain=true, l'état est disponible immédiatement)
- Publie sur `serre/cmd/{action}` pour envoyer des commandes

### 9.3 Continuité avec l'existant

Les pages de test (Recepteur_serre_dashboard.html et V2_Emetteur_serre_dashboard.html)
fonctionnent déjà avec HiveMQ + mqtt.js. Le design dark (DM Sans/DM Mono,
fond #0e1117, cartes avec bordure #2a313a) est validé.

### 9.4 Double accès

- **En local** : WiFi AP de la Waveshare → `http://192.168.5.1`
  (serveur web embarqué, pas de MQTT)
- **À distance** : GSM/4G/WiFi maison → PWA connectée au broker HiveMQ
  (MQTT over WebSocket)

### 9.5 Sécurité

Credentials MQTT visibles dans le code de la PWA.
Risque accepté : système personnel pour une serre privée.

---

## 10. SURVEILLANCE RUNTIME

### 10.1 Monitoring heap mémoire

L'ESP32-S3 cumule TLS (mbedTLS), AsyncWebServer et le client MQTT.
Risque de fragmentation heap et de crash après plusieurs jours.

**Mesure** : ajouter un log périodique dans une tâche TaskManager :
```cpp
Console::info("HEAP", "Free: " + String(heap_caps_get_free_size(MALLOC_CAP_8BIT))
              + " / Min: " + String(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)));
```

Fréquence : toutes les 5 minutes (même tâche que publishSystem).

**Seuil d'alerte** : si le minimum free descend sous ~50 KB, investiguer.
Les connexions TLS consomment ~40-50 KB temporairement pendant le handshake.

### 10.2 Monitoring tâches FreeRTOS

Surveiller le watermark (stack restant) des tâches créées :
```cpp
UBaseType_t hwm = uxTaskGetStackHighWaterMark(bridgeTaskHandle);
Console::info("BRIDGE", "Stack HWM: " + String(hwm * 4) + " bytes");
```

Si le watermark descend sous 512 bytes, augmenter la stack de la tâche.

---

## 11. PLAN D'IMPLÉMENTATION

### Phase 1 — BridgeManager + MqttManager Waveshare
1. Créer BridgeManager.h/.cpp :
   - Tâche FreeRTOS dédiée (§5.7)
   - ModemStateCache + spinlock (§5.3)
   - Flag SMS (§5.4)
   - Heartbeat avec flag (§5.5)
   - Accesseurs (§5.6)
2. Créer MqttManager.h/.cpp :
   - Machine d'états (§6.2)
   - Queue FreeRTOS pour commandes entrantes (§6.3)
   - Backoff avec jitter (§6.7)
3. Ajouter les defines dans NetworkConfig.h et TimingConfig.h
4. Intégrer dans main.cpp (init + tâches TaskManager)
5. Ajouter monitoring heap (§10.1)
6. Tester la connexion TLS vers HiveMQ

### Phase 2 — Endpoint modem-status LilyGo
1. Ajouter `GET /modem-status` dans le serveur HTTP existant
2. Exposer : ppp, mode, rssi_gsm, battery_mv, battery_pct, charging
3. Valider que le ModemStateCache se remplit correctement

### Phase 3 — Publication des données
1. Tâches périodiques publishSensors() et publishSystem()
2. Publication immédiate des événements critiques (vannes, erreurs, boot)
3. LWT retain + "online" explicite
4. Heartbeat régulé (un par cycle)

### Phase 4 — Réception commandes
1. Abonnement aux topics `serre/cmd/#`
2. Callback → queue FreeRTOS → dépilage dans handle()
3. Dispatch vers le module d'arrosage (futur)

### Phase 5 — PWA téléphone
1. Page web unique, mqtt.js, design dark existant
2. Cartes pour chaque DataId
3. Boutons de commande vannes
4. Indicateur online/offline via LWT retain

### Phase 6 — MQTT sur LilyGo (futur)
1. Ajout du client esp_mqtt dans le firmware LilyGo
2. Publication sur `serre/bridge/...`
3. Client ID `serre-bridge`
4. Coordination via BridgeManager étendu

---

## 12. FICHIERS DE RÉFÉRENCE

Documents fournis durant la conception :
- `Arborescence_projet.txt` — structure complète du projet Waveshare
- `main.cpp` (Waveshare) — setup/loop, orchestration, toutes les tâches
- `WiFiManager.cpp` — machine d'états WiFi (modèle pour MqttManager)
- `WebServer.cpp` — serveur web, routes, bundle download
- `PagePrincipale.cpp` — dashboard HTML, intégration DataLogger
- `EventManager.h` + `EventManager.cpp` — observation états, futur moteur de règles
- `main.cpp` (LilyGo) — firmware complet du pont WiFi-GSM
- `synthese_communication_lilygo_waveshare.txt` — protocole de communication
- `Recepteur_serre_dashboard.html` — prototype récepteur MQTT (mqtt.js)
- `V2_Emetteur_serre_dashboard.html` — prototype émetteur MQTT (mqtt.js)

---

## 13. RÈGLES DE DÉVELOPPEMENT

Préférences du développeur (à respecter impérativement) :
1. **Ne jamais coder avant accord** sur le design
2. **Ne jamais modifier de code en dehors du périmètre demandé**
3. **Toute hypothèse doit être expliquée et validée** avant implémentation
4. **Il est possible de dire "je ne sais pas"**
5. **Il est possible de se tromper** — on le dit et on rectifie après accord
6. **Aspect non-bloquant sacré** — le TaskManager ne doit jamais être bloqué
7. **Stabilité avant fonctionnalités** — on ne casse pas ce qui fonctionne

---

## 14. HISTORIQUE DES RÉVISIONS

### v1 (17 mars 2026)
- Design initial complet

### v2 (17 mars 2026)
- Intégration de 7 points issus de la revue externe :
  ModemStateCache, queue FreeRTOS, publication hybride,
  LWT retain, jitter backoff, timeout CONNECTING,
  coordination SMS via EventManager

### v3 (17 mars 2026)
- Remplacement coordination via EventManager par BridgeManager (module dédié)
- Clarification du rôle d'EventManager (futur moteur de règles)
- Prise en compte de l'EventManager existant (observateur d'état WiFi)

### v3.1 (17 mars 2026)
- BridgeManager::updateCache() déplacé dans une tâche FreeRTOS dédiée
  (suppression du dernier point de blocage potentiel dans TaskManager)
- Heartbeat régulé : un seul par cycle de publication, pas par message
- Ajout section surveillance runtime (heap, stack watermark)
- Ajout note QoS à revalider si commandes temporelles futures
- Formalisation de la règle : opération réseau bloquante = tâche FreeRTOS dédiée
