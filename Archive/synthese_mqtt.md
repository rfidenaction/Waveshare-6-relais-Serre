# SYNTHÈSE ARCHITECTURE MQTT — SERRE CONNECTÉE
## Document de référence pour implémentation

**Date** : 17 mars 2026
**Statut** : Validé — Prêt pour implémentation

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
  (sauf ajout du endpoint GET /modem-status, voir §6)

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
│   ├── EventManager.h/.cpp     // Gestion événements
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

**Pattern de la loop principale :**
```
setup() → loopInit() [une fois] → loopRun() [boucle infinie]
loopRun() appelle uniquement TaskManager::handle()
```

**Toutes les tâches** sont enregistrées dans TaskManager via `addTask(callback, period_ms)`.
Chaque callback doit retourner en < 50ms.

**DataLogger** : point central des données. Chaque module fait `DataLogger::push(DataType, DataId, value)`.
Les DataId ont des métadonnées (label, unité, nature) dans `DataMeta`.

**Serveur web actuel** : rendu côté serveur (HTML généré par C++), rafraîchissement par
`location.reload()` toutes les 30s, graphiques via `fetch('/graphdata')` → CSV.
Pas de WebSocket, pas de SSE.

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
serre/status/waveshare      // LWT : "online" / "offline" (automatique si déconnexion)
serre/status/bridge         // Réservé LilyGo future
```

### 4.2 QoS

| Type de message    | QoS | Justification |
|--------------------|-----|---------------|
| Données capteurs   | 0   | Perte acceptable, prochaine valeur dans 1h max |
| Commandes vannes   | 1   | Garantie de réception indispensable |
| LWT (status)       | 1   | Retain = true, doit persister |

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

## 5. MODULE MqttManager (Waveshare)

### 5.1 Emplacement

```
src/Connectivity/
├── MqttManager.h
└── MqttManager.cpp
```

Même pattern que WiFiManager : classe statique, machine d'états non-bloquante.

### 5.2 Machine d'états

```
IDLE
  │
  ▼ (WiFi STA connecté)
CHECK_MODEM
  │
  ├─ modem_status = DATA + ppp = up → CONNECTING
  ├─ modem_status = COMMAND → WAIT_MODEM (attendre 30s, réessayer)
  └─ HTTP timeout (LilyGo injoignable) → WAIT_RETRY
  │
  ▼
CONNECTING
  │ (esp_mqtt_client_start, non-bloquant)
  │
  ├─ MQTT_EVENT_CONNECTED → CONNECTED
  └─ timeout / erreur → WAIT_RETRY
  │
  ▼
CONNECTED
  │
  │ (publication normale, réception commandes)
  │
  ├─ MQTT_EVENT_DISCONNECTED → CHECK_MODEM (pas de panique, on vérifie d'abord)
  └─ demande SMS en cours → PAUSED
  │
  ▼
PAUSED
  │ (SMS en cours, on ne tente aucune reconnexion)
  │
  └─ SMS terminé → CHECK_MODEM
  │
  ▼
WAIT_MODEM
  │ (modem en mode COMMAND, on patiente)
  │
  └─ après 30s → CHECK_MODEM
  │
  ▼
WAIT_RETRY
  │ (backoff progressif : 15s → 30s → 60s → 120s → 120s plafond)
  │
  └─ délai écoulé → CHECK_MODEM
```

### 5.3 Intégration dans main.cpp

Dans `loopInit()`, après l'initialisation du WiFi et du TaskManager :

```cpp
// Initialisation MQTT (après WiFi, avant les tâches de données)
MqttManager::init();

// Tâche MQTT (machine d'états)
TaskManager::addTask(
    []() { MqttManager::handle(); },
    MQTT_HANDLE_PERIOD_MS       // ~1000ms suggéré
);
```

### 5.4 Intégration avec DataLogger

Deux approches possibles (à décider) :

**Option A — Hook dans DataLogger::push()**
DataLogger appelle `MqttManager::publish()` à chaque push, si connecté.
Avantage : chaque donnée est publiée dès qu'elle arrive.
Risque : couplage DataLogger ↔ MqttManager.

**Option B — Tâche périodique dédiée**
Une tâche TaskManager lit les dernières valeurs via `DataLogger::hasLastDataForWeb()`
et les publie périodiquement (toutes les 60s par exemple).
Avantage : découplé, simple, prévisible.
Inconvénient : latence de publication.

**Recommandation : Option B** — plus stable, découplé, conforme à la philosophie
"on ne modifie pas l'existant". La fréquence de publication peut être
ajustée par DataId (capteurs = 1h, WiFi status = 5min, etc.).

### 5.5 Réception des commandes

Quand un message arrive sur `serre/cmd/valve1`, le callback MQTT (exécuté dans
la tâche FreeRTOS d'esp_mqtt) doit **poster un flag/événement** vers le module
d'arrosage (ou EventManager), PAS exécuter l'action directement.

Ceci respecte la règle : les actions sont exécutées dans le contexte du TaskManager,
jamais dans un callback externe.

### 5.6 Configuration (NetworkConfig.h)

Nouveaux defines à ajouter :
```cpp
// MQTT Broker
#define MQTT_BROKER_URI     "mqtts://3db6155980d4483e8b8c3036fd0afd6f.s1.eu.hivemq.cloud:8883"
#define MQTT_USERNAME       "Graindesable"
#define MQTT_PASSWORD       "Chaperonrouge64"
#define MQTT_CLIENT_ID      "serre-waveshare"
#define MQTT_LWT_TOPIC      "serre/status/waveshare"
```

Nouveaux defines à ajouter dans TimingConfig.h :
```cpp
#define MQTT_HANDLE_PERIOD_MS           1000    // Fréquence machine d'états
#define MQTT_PUBLISH_SENSORS_PERIOD_MS  3600000 // Publication capteurs (1h)
#define MQTT_PUBLISH_SYSTEM_PERIOD_MS   300000  // Publication status WiFi etc. (5min)
#define MQTT_RECONNECT_BASE_MS          15000   // Backoff initial
#define MQTT_RECONNECT_MAX_MS           120000  // Backoff plafond
#define MQTT_MODEM_CHECK_TIMEOUT_MS     3000    // Timeout HTTP vers /modem-status
#define MQTT_MODEM_WAIT_MS              30000   // Attente si modem en COMMAND
```

---

## 6. COORDINATION WAVESHARE ↔ LILYGO

### 6.1 Principe

Le lien WiFi local (192.168.4.x) est **toujours disponible**, même quand internet
est coupé. C'est le canal de coordination.

### 6.2 Endpoint à ajouter sur la LilyGo

**`GET /modem-status`** — Seule modification de la LilyGo.

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
  - `DATA` = internet disponible
  - `COMMAND` = SMS en cours ou transition, internet coupé
- `rssi_gsm` : niveau signal cellulaire (dBm)
- `battery_mv` : tension batterie (millivolts, via pmu.getBattVoltage())
- `battery_pct` : pourcentage batterie (via pmu.getBatteryPercent())
- `charging` : état de charge (via pmu.isCharging())

**Risque minimal** : c'est une route HTTP en lecture seule, aucun effet de bord,
~15 lignes de code à ajouter dans le serveur HTTP existant de la LilyGo.

### 6.3 Scénarios de coordination

**Scénario 1 — La Waveshare veut publier en MQTT**
1. MqttManager vérifie `GET /modem-status`
2. Si `mode=DATA` et `ppp=up` → publie normalement
3. Si `mode=COMMAND` → état WAIT_MODEM, réessaie dans 30s

**Scénario 2 — La Waveshare veut envoyer un SMS**
1. Le SmsManager met MqttManager en état PAUSED
2. POST /sms vers la LilyGo
3. Attente retour PPP (~10-30s)
4. MqttManager sort de PAUSED → CHECK_MODEM → reconnexion

**Scénario 3 — La LilyGo envoie un SMS de son initiative (boot, alerte)**
1. La LilyGo bascule en COMMAND, envoie le SMS, revient en DATA
2. Pendant ce temps, le MqttManager perd sa connexion MQTT
3. Il interroge `/modem-status`, voit `mode=COMMAND` → WAIT_MODEM
4. Quand le mode revient à DATA → reconnexion automatique

**Scénario 4 — LilyGo injoignable (HTTP timeout sur /modem-status)**
1. Problème grave : la LilyGo est probablement en panne ou en reboot
2. MqttManager → WAIT_RETRY avec backoff progressif
3. Continue de réessayer indéfiniment

### 6.4 Heartbeat existant (inchangé)

Le `GET /heartbeat` vers `http://192.168.4.1/heartbeat` reste en place.
La Waveshare l'appelle après chaque publication MQTT réussie.
Ça confirme à la LilyGo que **toute la chaîne** fonctionne :
WiFi Waveshare → NAT LilyGo → Internet → Broker → retour OK.

---

## 7. APPLICATION TÉLÉPHONE (PWA)

### 7.1 Approche

Progressive Web App (PWA) — page web qui se connecte au broker HiveMQ
via MQTT over WebSocket (`mqtt.js`). Installable sur Android comme une app.

### 7.2 Fonctionnement

- Se connecte à `wss://...hivemq.cloud:8884/mqtt` avec les mêmes credentials
- S'abonne à `serre/data/#` pour recevoir toutes les données capteurs
- S'abonne à `serre/bridge/#` pour les données du pont (futur)
- S'abonne à `serre/status/#` pour savoir si les cartes sont en ligne
- Publie sur `serre/cmd/{action}` pour envoyer des commandes

### 7.3 Continuité avec l'existant

Les pages de test (Recepteur_serre_dashboard.html et V2_Emetteur_serre_dashboard.html)
fonctionnent déjà avec HiveMQ + mqtt.js. Le design dark (DM Sans/DM Mono,
fond #0e1117, cartes avec bordure #2a313a) est validé.

La PWA finale reprendra ce design et ajoutera les cartes correspondant
à chaque DataId publiée par la Waveshare.

### 7.4 Double accès

L'utilisateur a deux moyens d'accéder à l'interface :
- **En local** : connecté au WiFi AP de la Waveshare → `http://192.168.5.1`
  (serveur web embarqué existant, pas de MQTT nécessaire)
- **À distance** : via GSM/4G/WiFi maison → PWA connectée au broker HiveMQ
  (MQTT over WebSocket, mêmes données, même interface)

---

## 8. PLAN D'IMPLÉMENTATION

### Phase 1 — MqttManager Waveshare (priorité)
1. Ajouter les defines dans NetworkConfig.h et TimingConfig.h
2. Créer MqttManager.h/.cpp (machine d'états, esp_mqtt natif)
3. Intégrer dans main.cpp (init + tâche TaskManager)
4. Tâche de publication périodique (lit DataLogger, publie sur MQTT)
5. Tester la connexion TLS vers HiveMQ

### Phase 2 — Endpoint modem-status LilyGo
1. Ajouter `GET /modem-status` dans le serveur HTTP existant
2. Exposer : ppp, mode, rssi_gsm, battery_mv, battery_pct, charging
3. Intégrer la vérification dans MqttManager (CHECK_MODEM)

### Phase 3 — Réception commandes
1. Abonnement aux topics `serre/cmd/#`
2. Callback → flag/événement vers EventManager
3. Intégration avec le module d'arrosage (futur)

### Phase 4 — PWA téléphone
1. Page web unique, mqtt.js, design dark existant
2. Cartes pour chaque DataId
3. Boutons de commande vannes
4. Indicateur online/offline via LWT

### Phase 5 — MQTT sur LilyGo (futur)
1. Ajout du client esp_mqtt dans le firmware LilyGo
2. Publication sur `serre/bridge/...`
3. Client ID `serre-bridge`
4. Coordination via mutex modem existant (déjà en place pour SMS)

---

## 9. FICHIERS DE RÉFÉRENCE

Documents fournis durant la conception :
- `Arborescence_projet.txt` — structure complète du projet Waveshare
- `main.cpp` (Waveshare) — setup/loop, orchestration, toutes les tâches
- `WiFiManager.cpp` — machine d'états WiFi (modèle pour MqttManager)
- `WebServer.cpp` — serveur web, routes, bundle download
- `PagePrincipale.cpp` — dashboard HTML, intégration DataLogger
- `main.cpp` (LilyGo) — firmware complet du pont WiFi-GSM
- `synthese_communication_lilygo_waveshare.txt` — protocole de communication
- `Recepteur_serre_dashboard.html` — prototype récepteur MQTT (mqtt.js)
- `V2_Emetteur_serre_dashboard.html` — prototype émetteur MQTT (mqtt.js)

---

## 10. RÈGLES DE DÉVELOPPEMENT

Préférences du développeur (à respecter impérativement) :
1. **Ne jamais coder avant accord** sur le design
2. **Ne jamais modifier de code en dehors du périmètre demandé**
3. **Toute hypothèse doit être expliquée et validée** avant implémentation
4. **Il est possible de dire "je ne sais pas"**
5. **Il est possible de se tromper** — on le dit et on rectifie après accord
6. **Aspect non-bloquant sacré** — le TaskManager ne doit jamais être bloqué
7. **Stabilité avant fonctionnalités** — on ne casse pas ce qui fonctionne
