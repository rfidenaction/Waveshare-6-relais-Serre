# SYNTHÈSE ARCHITECTURE MQTT — SERRE CONNECTÉE
## Document de référence pour implémentation — Version 4.3

**Date** : 28 mars 2026
**Statut** : Validé — Phase 1 implémentée (MqttManager + BridgeManager + SmsManager)
**Révision** : v4.3 — BridgeManager UDP implémenté, SmsManager refactoré, heartbeat MQTT couplé

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
- RTC intégrée (horodatage fiable dès le boot, indépendant du NTP)

**LilyGo T-SIM7080G-S3** (pont WiFi-GSM)
- Rôle : pont NAT WiFi → Cat-M (PPPoS) vers internet + service SMS
- WiFi SoftAP : `192.168.4.1`, SSID `Pont_Wifi-GSM_de_la_serre`, canal 1, 1 client max
- Modem : SIM7080G, APN `domotec82.fr`
- PMU : AXP2101 (batterie intégrée, voltage/charge lisibles)
- Framework : ESP-IDF pur (FreeRTOS)
- **Ce firmware doit être modifié pour ajouter le listener UDP** (voir §7)

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
    WiFi AP 192.168.5.1
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
│   ├── BridgeManager.h/.cpp    // Communication UDP vers LilyGo (heartbeat, SMS, cache état)
│   ├── MqttManager.h/.cpp      // Client MQTT réactif (esp_mqtt natif)
│   └── SmsManager.h/.cpp       // Logique métier SMS (délègue transport à BridgeManager)
├── Core/
│   ├── TaskManager.h/.cpp      // Tâches périodiques (cœur du système)
│   ├── EventManager.h/.cpp     // Observation états + futur moteur de règles
│   └── SafeReboot.h/.cpp       // Reboot préventif automatique
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

**Toutes les données sont horodatées par la Waveshare** (UTC). Le DataLogger
utilise `ManagerUTC::nowUtc()` dès que le NTP est disponible. Avec la RTC
intégrée, l'horodatage est fiable dès le boot.

**Serveur web actuel** : rendu côté serveur (HTML généré par C++), rafraîchissement
par `location.reload()` toutes les 30s, graphiques via `fetch('/graphdata')` → CSV.

### 1.6 Tâches FreeRTOS indépendantes du TaskManager

Un seul composant tourne dans une tâche FreeRTOS dédiée :

| Composant          | Tâche FreeRTOS        | Raison                                    |
|--------------------|-----------------------|-------------------------------------------|
| esp_mqtt           | créée par esp_mqtt    | TLS handshake, reconnexion, callbacks      |

esp_mqtt communique avec le TaskManager via le flag `mqttConnected`.
Il ne touche JAMAIS directement la logique métier.

**Note importante** : BridgeManager n'utilise **PAS** de tâche FreeRTOS.
Le transport UDP est non-bloquant (quelques microsecondes par appel),
donc BridgeManager tourne directement dans le TaskManager.

~~L'ancienne version (v4.2) prévoyait une tâche FreeRTOS pour BridgeManager
car le transport était HTTP (bloquant). Le passage à UDP a supprimé ce besoin.~~

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
- Limite : 100 connexions simultanées (largement suffisant)
- Limite : 10 GB/mois de débit (largement suffisant pour ~20 valeurs/heure)
- TLS requis → le client ESP32 doit utiliser un certificat CA racine
  (ISRG Root X1 ou équivalent selon la chaîne HiveMQ)
- **Client IDs doivent être uniques** : HiveMQ déconnecte un client
  si un autre se connecte avec le même ID
- Le plan Serverless ne propose **pas** d'intégration directe vers des bases
  de données (réservé aux plans payants). D'où la nécessité du VPS (voir §9).

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
- `serre/data/supply_voltage` → `1710684600,0,0,0,24.120`
- `serre/data/air_temperature_1` → `1710684600,1,1,0,23.500`
- `serre/data/air_humidity_1` → `1710684600,1,2,0,67.200`
- `serre/data/soil_moisture_1` → `1710684600,1,3,0,45.000`
- `serre/data/valve1` → `1710684600,2,4,0,1.000`
- `serre/data/wifi_sta_connected` → `1710684600,3,5,0,1.000`
- `serre/data/wifi_rssi` → `1710684600,3,7,0,-67.000`
- `serre/data/boot` → `1710684600,3,8,1,"Démarrage normal"`

Les labels utilisés dans les topics sont ceux de DataMeta (identifiants techniques).
Le mapping vers les labels "en clair" (pour affichage) est défini dans
la table META de DataLogger.h et transmis automatiquement via le schéma (voir §4.5).

**Données LilyGo → Broker (FUTUR, pas implémenté maintenant)**
```
serre/bridge/{label}        // Réservé pour la LilyGo
```
Exemples futurs :
- `serre/bridge/battery_voltage` → `1710684600,0,0,0,3.850`
- `serre/bridge/battery_percent` → `1710684600,0,1,0,72.000`
- `serre/bridge/charging` → `1710684600,0,2,0,1.000`
- `serre/bridge/gsm_rssi` → `1710684600,0,3,0,-67.000`
- `serre/bridge/ppp_status` (format à définir)
- `serre/bridge/modem_mode` (format à définir)

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

**Schéma des métadonnées (auto-publié)**
```
serre/schema                // Schéma JSON auto-généré depuis META
```
- Publié automatiquement **à chaque boot** de la Waveshare (après connexion MQTT)
- **retain = true, QoS 1** — le broker conserve la dernière version
- Tout nouveau client (PWA, VPS) reçoit le schéma immédiatement à la connexion
- Voir §4.5 pour le format et le mécanisme de détection de changement

### 4.2 QoS

| Type de message    | QoS | Justification |
|--------------------|-----|---------------|
| Données capteurs   | 0   | Perte acceptable, prochaine valeur dans 1h max |
| Commandes vannes   | 1   | Garantie de réception indispensable |
| LWT (status)       | 1   | Retain = true, doit persister |
| Schéma (schema)    | 1   | Retain = true, doit persister |

Note sur QoS 1 : assure "at-least-once", pas "exactly-once". Une commande
de vanne peut arriver deux fois. Ceci est sans conséquence car une vanne
est un état (ouvert/fermé), pas une action incrémentale.

⚠️ **À revalider** si le futur moteur de règles introduit des commandes
temporelles (ex: "ouvrir 2 minutes"). Dans ce cas, une duplication
serait un bug. On adaptera le protocole à ce moment-là.

### 4.3 Client IDs

| Carte / Service | Client ID            |
|-----------------|----------------------|
| Waveshare       | `serre-waveshare`    |
| LilyGo          | `serre-bridge` (réservé, pas utilisé maintenant) |
| VPS             | `serre-vps`          |
| PWA téléphone 1 | `serre-pwa-{random}` (généré à chaque connexion) |

Les PWA utilisent un suffixe aléatoire car plusieurs téléphones peuvent
se connecter simultanément. HiveMQ déconnecterait un client si un autre
utilisait le même ID.

### 4.4 Format des payloads données

**Format : ligne CSV identique au stockage SPIFFS du DataLogger.**

```
timestamp,type,id,valueType,value
```

| Champ      | Type    | Description |
|------------|---------|-------------|
| timestamp  | uint32  | Timestamp UTC en secondes (horodaté par la Waveshare) |
| type       | uint8   | DataType (0=Power, 1=Sensor, 2=Actuator, 3=System) |
| id         | uint8   | DataId (index dans l'enum, 0 à Count-1) |
| valueType  | uint8   | 0 = float, 1 = texte |
| value      | string  | Valeur : float formaté (ex: `23.500`) ou texte échappé CSV (ex: `"Reboot watchdog"`) |

**Exemples :**
```
1710684600,1,1,0,23.500        // Température air 1 = 23.5°C
1710684600,2,4,0,1.000         // Vanne 1 = ouverte
1710684600,3,8,1,"Démarrage"   // Boot = "Démarrage"
```

**Justification du choix :**
- Format identique entre SPIFFS, MQTT et stockage VPS = un seul parser partout
- Le payload est **autoportant** : il contient toutes les informations nécessaires
  pour être interprété sans connaître le topic
- Le topic sert uniquement au routage (abonnement, filtrage côté broker)
- La logique de formatage dans MqttManager réutilise le même code que
  `flushToFlash()` dans DataLogger.cpp

**Règle : on n'envoie et on ne manipule QUE des données horodatées par la Waveshare.**
Le timestamp est celui de l'acquisition, pas celui de la réception par le broker
ou par un consommateur. L'horloge source est la RTC de la Waveshare, synchronisée
via NTP.

**Spécification stricte du format CSV (contrat d'encodage) :**

Ce format est utilisé en SPIFFS, en MQTT et en stockage VPS. Tout parser
(C++, JavaScript, Python) doit respecter les mêmes règles.

- Séparateur : virgule `,` — jamais d'espace avant ou après
- Encodage : UTF-8
- Un message MQTT = une ligne CSV = un enregistrement (pas de retour à la ligne dans une valeur)
- Les 4 premiers champs (timestamp, type, id, valueType) sont **toujours numériques**,
  jamais entre guillemets, jamais vides
- Si valueType = 0 (float) : le champ value est un nombre décimal en ASCII,
  point `.` comme séparateur décimal, jamais de guillemets
  (ex: `23.500`, `-67.000`, `1.000`)
- Si valueType = 1 (texte) : le champ value est **obligatoirement** entre
  guillemets doubles `"..."`. Les guillemets internes sont doublés `""`
  (convention CSV standard RFC 4180). Ex: `"Démarrage"`, `"Erreur ""timeout"""`
- Un `split(",")` naïf ne suffit PAS pour parser ce format : une virgule
  à l'intérieur d'un champ texte entre guillemets n'est pas un séparateur.
  Les parsers doivent gérer les guillemets correctement.
- Implémentation de référence côté firmware : `escapeCSV()` / `unescapeCSV()`
  dans DataLogger.cpp

### 4.5 Schéma auto-publié (serre/schema)

Le schéma est publié sur `serre/schema` avec retain=true à chaque boot de la
Waveshare. Il contient les métadonnées complètes de tous les DataId, générées
automatiquement depuis la table META de DataLogger.h.

**Format JSON :**
```json
{
  "version": "1",
  "hash": "A3F2B7C1",
  "generated": "17-03-2026 14:30:00",
  "dataTypes": [
    {"id": 0, "label": "Power"},
    {"id": 1, "label": "Sensor"},
    {"id": 2, "label": "Actuator"},
    {"id": 3, "label": "System"}
  ],
  "dataIds": [
    {"id": 0, "label": "Tension alim", "unit": "V", "nature": "metrique", "type": 0},
    {"id": 1, "label": "Température air 1", "unit": "°C", "nature": "metrique", "type": 1},
    {"id": 2, "label": "Humidité air 1", "unit": "%", "nature": "metrique", "type": 1},
    {"id": 3, "label": "Humidité sol 1", "unit": "%", "nature": "metrique", "type": 1},
    {"id": 4, "label": "Vanne 1", "unit": "", "nature": "etat", "type": 2,
     "states": [{"value": 0, "label": "Fermée"}, {"value": 1, "label": "Ouverte"}]},
    {"id": 5, "label": "WiFi STA", "unit": "", "nature": "etat", "type": 3,
     "states": [{"value": 0, "label": "Déconnecté"}, {"value": 1, "label": "Connecté"}]},
    {"id": 6, "label": "WiFi AP", "unit": "", "nature": "etat", "type": 3,
     "states": [{"value": 0, "label": "Inactif"}, {"value": 1, "label": "Actif"}]},
    {"id": 7, "label": "WiFi RSSI", "unit": "dBm", "nature": "metrique", "type": 3},
    {"id": 8, "label": "Démarrage", "unit": "", "nature": "texte", "type": 3},
    {"id": 9, "label": "Erreur", "unit": "", "nature": "texte", "type": 3}
  ]
}
```

**Champ `hash`** : CRC32 calculé sur le contenu du schéma JSON (hors champs
`hash` et `generated` eux-mêmes). Permet aux consommateurs de détecter un
changement de schéma sans comparer le JSON complet. CRC32 est disponible
nativement dans ESP-IDF (`esp_rom_crc.h`).

**Génération** : la même logique que `buildBundleHeader()` dans WebServer.cpp
est réutilisée par MqttManager. La table META reste la **source de vérité unique**.

**Cycle de vie du schéma :**
1. Le développeur modifie META dans DataLogger.h (ajout DataId, changement label...)
2. Recompilation + flash → l'ESP32 reboot
3. MqttManager se connecte au broker
4. MqttManager génère le schéma JSON, calcule le CRC32, publie sur `serre/schema`
   (retain=true, QoS 1)
5. Le VPS compare le hash reçu avec son hash en cache :
   - Différent → met à jour sa copie locale, log l'événement
   - Identique → ignore
6. La PWA compare le hash reçu avec son hash en cache :
   - Différent → met à jour son cache, rafraîchit l'affichage
   - Identique → ignore

Le schéma n'est publié qu'au reboot. Pas de republication périodique.

**Aucune intervention manuelle** n'est nécessaire après un flash.

---

## 5. BRIDGEMANAGER — COORDINATION WAVESHARE ↔ LILYGO

### 5.1 Rôle et périmètre — ✅ IMPLÉMENTÉ (v4.3)

BridgeManager est le **point unique** de communication entre les deux cartes.
Tout ce qui touche à la LilyGo passe par lui. Ni MqttManager, ni SmsManager
ne communiquent directement avec la LilyGo.

**Responsabilités :**
- Transport UDP bidirectionnel vers la LilyGo
- Maintenir le cache d'état de la LilyGo (passivement, via paquets STATE)
- Queue et envoi des SMS (2 slots max, avec ACK)
- Heartbeat couplé au métier MQTT (signe de vie après 5 publications)
- Logger tous les événements SMS (Console + DataLogger → MQTT automatique)
- Exposer les données LilyGo (batterie, RSSI GSM) au reste du système

**Ce n'est PAS sa responsabilité :**
- Logique métier SMS (quand envoyer, à qui) → SmsManager
- Logique métier (→ futur moteur de règles dans EventManager)
- Gestion WiFi (→ WiFiManager)
- Protocole MQTT (→ MqttManager)

### 5.2 Transport : UDP unicast

**Pourquoi UDP et pas HTTP** : la radio WiFi est partagée entre AP et STA.
En mode AP+STA, un cycle HTTP complet (connexion TCP + requête + réponse)
dépasse systématiquement 100ms, souvent 200-300ms. Ceci bloque le TaskManager
et provoque des fuites de sockets (errno 11). **Le HTTP vers la LilyGo
a été testé et abandonné** (voir historique v4.3).

L'UDP résout tous ces problèmes :
- Pas de handshake TCP → un `sendto()` prend quelques millisecondes
- Un `recvfrom()` non-bloquant retourne immédiatement s'il n'y a rien
- Un seul socket ouvert une fois au boot, réutilisé indéfiniment
- Pas de fuite de sockets

```
Waveshare (192.168.4.10:5001) ←──UDP──→ LilyGo (192.168.4.1:5000)
```

Ports définis dans `NetworkConfig.h` :
```cpp
static constexpr uint16_t BRIDGE_UDP_PORT_LOCAL  = 5001;  // Waveshare écoute
static constexpr uint16_t BRIDGE_UDP_PORT_REMOTE = 5000;  // LilyGo écoute
```

### 5.3 Protocole UDP (4 types de paquets)

**Waveshare → LilyGo :**

| Paquet | Format | Signification |
|--------|--------|---------------|
| Heartbeat | `HB` | Signe de vie — la Waveshare est vivante et produit des données |
| Ordre SMS | `SMS\|id\|number\|text` | Demande d'envoi de SMS via le modem |

**LilyGo → Waveshare :**

| Paquet | Format | Signification |
|--------|--------|---------------|
| État | `STATE\|canSms\|ppp\|rssi\|bat` | État périodique (~5s) |
| ACK SMS | `ACK\|id` | Le SMS a été envoyé par le modem |

**Exemples :**
```
HB                              → Heartbeat
SMS|3|+33672967933|Bonjour       → Ordre SMS id:3
STATE|1|1|-67|78                 → canSms=true, ppp=up, rssi=-67dBm, bat=78%
ACK|3                            → SMS id:3 envoyé avec succès
```

Format pipe-séparé : léger, facile à parser, pas de dépendance JSON.

### 5.4 Cache état LilyGo

Structure mise à jour passivement par les paquets STATE reçus de la LilyGo.
Aucune requête nécessaire — la LilyGo envoie son état toutes les ~5s.

```cpp
struct LilyGoState {
    bool     canAcceptSms;      // LilyGo peut accepter un ordre SMS
    bool     pppUp;             // Connexion PPP active
    int      rssiGsm;           // Signal cellulaire (dBm)
    int      batteryPct;        // Pourcentage batterie (interpolation 3.0–4.2V)
    uint32_t lastReceivedMs;    // millis() du dernier paquet STATE reçu
};
```

**Accesseurs publics (lecture instantanée, < 1µs, aucun appel réseau) :**
```cpp
static bool canAcceptSms();     // LilyGo peut accepter un ordre SMS
static bool isPppUp();          // Connexion PPP active
static int  getGsmRssi();       // Signal cellulaire (dBm)
static int  getBatteryPct();    // Pourcentage batterie
static bool isReachable();      // STATE reçu < 30s (LilyGo joignable)
```

**Flag `canAcceptSms`** : c'est la LilyGo qui gère son propre rate limit,
l'état de sa queue SMS, et la disponibilité du modem. Elle expose le résultat
dans le champ `canSms` du paquet STATE. La Waveshare ne fait que lire ce flag
— elle ne connaît rien du modem.

### 5.5 Heartbeat — couplé au métier MQTT

Le heartbeat n'est **pas** périodique (pas de timer). Il est envoyé après
**5 publications MQTT réussies**. Ceci signifie "la Waveshare est vivante
ET elle produit des données". Si le métier ne s'exécute plus (capteurs en
panne, DataLogger bloqué), le heartbeat s'arrête — c'est l'alerte la plus
importante pour la LilyGo.

**Chaîne de callbacks (même pattern que DataLogger → MqttManager) :**
```
DataLogger::push()
  → MqttManager::onDataPushed()     (callback DataLogger)
    → esp_mqtt_client_publish()
      → si réussi : _onPublishSuccess()
        → BridgeManager::onMqttPublish()   (callback MqttManager)
          → publishCounter++
          → si >= 5 : sendHeartbeat(), publishCounter = 0
```

Tout s'exécute dans le contexte du TaskManager. Pas de thread safety nécessaire.

### 5.6 SMS — Queue et machine d'états

**Queue** : 2 slots fixes. Si la queue est pleine, le nouveau SMS est
**rejeté** (pas d'écrasement) et l'événement est loggé.

**Machine d'états** (dans `handle()`, appelé toutes les 500ms) :

```
IDLE
  └─ SMS en queue + canAcceptSms + isReachable → envoi UDP → WAIT_ACK

WAIT_ACK
  ├─ ACK reçu → succès, log, supprime SMS → IDLE
  └─ Timeout 3 min :
       ├─ tentative 1 + canAcceptSms → retry → WAIT_ACK
       ├─ tentative 1 + !canAcceptSms → abandon + log → IDLE
       └─ tentative 2 → abandon + log → IDLE
```

**Signification de l'ACK** : le SMS a été **envoyé avec succès par le modem** (pas juste
accepté en queue). La LilyGo envoie l'ACK quand elle a la confirmation du
modem. Si le SMS échoue côté modem, la LilyGo ne renvoie pas d'ACK — le
timeout de 3 minutes côté Waveshare gère ce cas.

**Pas de cooldown entre SMS** : c'est la LilyGo qui gère son rate limit via
le flag `canAcceptSms`. Si elle est prête, on envoie immédiatement le suivant.

**Logging** : chaque événement SMS est loggé dans Console (série) ET dans
DataLogger via `DataLogger::push(DataId::Error, "[SMS] ...")`, ce qui
déclenche automatiquement la publication MQTT.

### 5.7 Démarrage différé (4 minutes)

BridgeManager a un **démarrage différé** de 4 minutes (`BRIDGE_START_DELAY_MS`).
Pendant ce délai, `handle()` retourne immédiatement sans rien faire.

**Pourquoi** : le socket UDP ouvert au boot perturbait le NTP. La radio WiFi
est partagée entre AP et STA, et un socket en écoute pendant la phase de
stabilisation (WiFi STA, MQTT TLS handshake, NTP) provoquait des timeouts
NTP répétés (5 essais au lieu de 1). En retardant l'ouverture du socket UDP
de 4 minutes, tout se stabilise d'abord.

**Séquence au boot :**
1. `init()` → variables initialisées, pas de socket
2. `handle()` pendant 4 min → retour immédiat
3. Premier `handle()` après 4 min → `udp.begin()`, log "Démarré"
4. `handle()` suivants → `processIncoming()` + `handleSmsMachine()`

### 5.8 Intégration dans main.cpp — ✅ IMPLÉMENTÉ

```cpp
// Initialisation (après SafeReboot, avant MqttManager)
BridgeManager::init();

// MQTT avec chaîne de callbacks
MqttManager::init();
DataLogger::setOnPush(MqttManager::onDataPushed);
MqttManager::setOnPublishSuccess(BridgeManager::onMqttPublish);

// SmsManager (logique métier SMS)
SmsManager::init();

// Tâche TaskManager (500ms)
TaskManager::addTask(
    []() { BridgeManager::handle(); },
    BRIDGE_HANDLE_PERIOD_MS
);

// SmsManager (2s)
TaskManager::addTask(
    []() { SmsManager::handle(); },
    2000
);
```

### 5.9 Configuration

**NetworkConfig.h :**
```cpp
static constexpr uint16_t BRIDGE_UDP_PORT_LOCAL  = 5001;
static constexpr uint16_t BRIDGE_UDP_PORT_REMOTE = 5000;
```

**TimingConfig.h :**
```cpp
#define BRIDGE_START_DELAY_MS       240000UL    // 4 minutes (démarrage différé)
#define BRIDGE_HANDLE_PERIOD_MS     500         // Période handle() TaskManager
#define BRIDGE_SMS_ACK_TIMEOUT_MS   180000UL    // 3 minutes (timeout ACK SMS)
#define BRIDGE_REACHABLE_TIMEOUT_MS 30000UL     // 30 secondes (seuil injoignable)
```

---

## 6. MODULE MqttManager (Waveshare)

### 6.1 Emplacement

```
src/Connectivity/
├── MqttManager.h
└── MqttManager.cpp
```

Même pattern que WiFiManager : classe statique. Mais contrairement à
WiFiManager, MqttManager n'a **pas de machine d'états** ni de `handle()`.
Il est purement **réactif** : déclenché par le callback `onDataPushed()`
de DataLogger à chaque nouvelle donnée. esp_mqtt gère sa propre tâche
FreeRTOS pour la connexion TLS, le keepalive et la reconnexion automatique.

### 6.2 Architecture réactive (implémenté)

MqttManager n'a **pas de machine d'états** et **pas de tâche TaskManager**.
Il repose sur deux mécanismes :

**1. Démarrage conditionnel — `ensureMqttStarted()`**

```
init()
  │ configure esp_mqtt (broker, TLS, LWT, credentials, event handler)
  │ mais NE fait PAS esp_mqtt_client_start()
  │ mqttStarted = false
  │
  ▼
onDataPushed() — appelé à chaque DataLogger::push()
  │
  ├─ ensureMqttStarted()
  │     si mqttStarted → return (coût = un test de booléen)
  │     si !WiFiManager::isSTAConnected() → return
  │     sinon → esp_mqtt_client_start(), mqttStarted = true
  │
  ├─ si !mqttConnected → return (esp_mqtt pas encore connecté au broker)
  │
  ├─ publication du message sur serre/data/{id}
  │
  └─ si publication réussie → _onPublishSuccess() (notification BridgeManager)
```

**2. Gestion de la connexion par esp_mqtt (tâche FreeRTOS interne)**

esp_mqtt gère seul :
- Le handshake TLS vers le broker
- Le keepalive (PINGREQ/PINGRESP toutes les 90 secondes)
- La reconnexion automatique après une coupure
- Les callbacks événementiels (CONNECTED, DISCONNECTED, DATA, ERROR)

Le callback `mqttEventHandler()` met à jour le flag `mqttConnected` et
gère les publications initiales (online, schéma) à la connexion.

**Séquence type au boot :**
1. `init()` → client configuré, en attente
2. WiFi STA pas encore connecté → les `onDataPushed()` sortent immédiatement
3. WiFi STA se connecte (~5-70s après boot)
4. Premier `DataLogger::push()` avec WiFi STA actif → `ensureMqttStarted()`
   fait le `start()` → esp_mqtt lance sa tâche FreeRTOS
5. Handshake TLS (~3s) → callback CONNECTED → `mqttConnected = true`
6. Les `onDataPushed()` suivants publient normalement

**Nommage des variables d'état :**
- `mqttStarted` : `esp_mqtt_client_start()` a été appelé (tâche FreeRTOS lancée)
- `mqttConnected` : connexion TCP/TLS active vers le broker (publications possibles)
- `isMqttConnected()` : accesseur public pour `mqttConnected`

**Callback publication réussie (v4.3) :**
- `setOnPublishSuccess(callback)` — même pattern que `DataLogger::setOnPush()`
- Appelé dans `onDataPushed()` après chaque publication MQTT réussie (`msgId >= 0`)
- Utilisé par BridgeManager pour le compteur heartbeat
- S'exécute dans le contexte du TaskManager (pas de thread safety)

### 6.3 Queue de commandes entrantes (thread safety) — FUTUR Phase 4

**Non implémenté.** L'abonnement à `serre/cmd/#` est en place, mais le
dépilage via queue FreeRTOS sera ajouté en Phase 4 (réception commandes).

Les callbacks `esp_mqtt` s'exécutent dans la tâche FreeRTOS du client MQTT,
**PAS** dans le contexte du TaskManager. Accéder directement à des données
partagées depuis ce callback créerait des data races.

**Solution prévue : queue FreeRTOS thread-safe**

```
esp_mqtt callback (tâche esp_mqtt)
    │
    ▼
xQueueSend(mqttIncomingQueue, &message)
    │
    ▼
Dépilage (mécanisme à définir en Phase 4)
    │
    ▼
Action selon topic (commande vanne, etc.)
```

La queue sera créée dans `MqttManager::init()`.
Le callback ne fait QUE poster dans la queue.
Le mécanisme de dépilage sera défini en Phase 4. MqttManager n'ayant
pas de `handle()`, le dépilage pourra se faire via une micro-tâche
TaskManager dédiée aux commandes, ou via un autre mécanisme adapté.

### 6.4 Intégration dans main.cpp — ✅ IMPLÉMENTÉ (v4.3)

Dans `loopInit()` :
```cpp
// Initialisation MQTT (configure le client, ne démarre pas)
MqttManager::init();

// Chaîne de callbacks : DataLogger → MQTT → BridgeManager
DataLogger::setOnPush(MqttManager::onDataPushed);
MqttManager::setOnPublishSuccess(BridgeManager::onMqttPublish);
```

**Aucune tâche TaskManager pour MqttManager.** Le démarrage et la
publication sont entièrement pilotés par le callback `onDataPushed()`
déclenché à chaque `DataLogger::push()`.

Le premier `esp_mqtt_client_start()` se produit au premier push
après la connexion WiFi STA (typiquement via la tâche WiFi status
qui pousse `WifiStaConnected` régulièrement).

### 6.5 Stratégie de publication (implémenté)

**Publication immédiate via callback** : chaque appel à `DataLogger::push()`
déclenche `MqttManager::onDataPushed()`. Si MQTT est connecté, le message
est publié immédiatement sur `serre/data/{id}`. C'est le rythme des
capteurs et du DataLogger qui dicte les publications, pas MqttManager.

- **Format du payload** : CSV 7 champs identique au SPIFFS (§4.4)
  ```
  timestamp,UTC_available,UTC_reliable,type,id,valueType,value
  ```
- La logique de formatage réutilise le même pattern que `flushToFlash()`
  dans DataLogger.cpp
- Si MQTT n'est pas connecté, la donnée n'est pas publiée mais reste
  stockée en SPIFFS par DataLogger (aucune perte locale)

**Publication du schéma** (§4.5) :
- Au boot, après la première connexion MQTT réussie (callback CONNECTED)
- Flag `schemaPublished` pour ne publier qu'une fois par session

**Publication "online"** :
- À chaque connexion au broker (callback CONNECTED)
- Retain = true sur `serre/status/waveshare`

### 6.6 Configuration (NetworkConfig.h)

```cpp
// MQTT Broker
static constexpr const char* MQTT_BROKER_URI   = "mqtts://...hivemq.cloud:8883";
static constexpr const char* MQTT_USERNAME     = "Graindesable";
static constexpr const char* MQTT_PASSWORD     = "Chaperonrouge64";
static constexpr const char* MQTT_CLIENT_ID    = "serre-waveshare";
static constexpr const char* MQTT_LWT_TOPIC    = "serre/status/waveshare";
static constexpr const char* MQTT_SCHEMA_TOPIC = "serre/schema";
static constexpr int         MQTT_KEEPALIVE_S  = 90;
```

### 6.7 Configuration (TimingConfig.h)

```cpp
// MqttManager — pas de defines spécifiques
// Le keepalive est dans NetworkConfig.h (MQTT_KEEPALIVE_S = 90)
// La reconnexion est gérée par esp_mqtt en interne

// BridgeManager — voir §5.9
```

~~Anciens defines supprimés (v4.2)~~ : `MQTT_HANDLE_PERIOD_MS`,
`MQTT_CONNECT_TIMEOUT_MS`, `MQTT_PUBLISH_SENSORS_PERIOD_MS`,
`MQTT_PUBLISH_SYSTEM_PERIOD_MS`, `MQTT_RECONNECT_BASE_MS`,
`MQTT_RECONNECT_MAX_MS`, `MQTT_RECONNECT_JITTER_PCT`,
`MQTT_SCHEMA_REPUBLISH_PERIOD_MS` — tous liés à l'ancienne machine
d'états et aux tâches périodiques.

~~Anciens defines BridgeManager supprimés (v4.3)~~ : `BRIDGE_CACHE_UPDATE_PERIOD_MS`,
`BRIDGE_HTTP_TIMEOUT_MS`, `BRIDGE_MODEM_WAIT_MS`, `BRIDGE_TASK_STACK`,
`BRIDGE_TASK_PRIORITY`, `BRIDGE_TASK_CORE` — tous liés à l'ancienne
architecture HTTP + tâche FreeRTOS.

---

## 7. MODIFICATIONS LILYGO — PROTOCOLE UDP

### 7.1 Vue d'ensemble

La LilyGo doit ajouter un **listener UDP** et un **émetteur d'état périodique**.
Ces ajouts ne modifient pas les tâches existantes (`sms_task`, `ppp_reconnect_task`,
gestion modem).

**À ajouter :**
1. Tâche FreeRTOS UDP listener (port 5000) — reçoit HB et SMS
2. Envoi périodique de l'état (STATE) en UDP vers la Waveshare (toutes les ~5s)
3. Envoi ACK quand un SMS a été envoyé par le modem

**À NE PAS modifier :**
- `sms_task` (envoi SMS via le modem)
- `ppp_reconnect_task` (gestion PPP)
- Gestion modem (AT commands)

### 7.2 Paquets reçus par la LilyGo

**`HB`** — Heartbeat de la Waveshare. La LilyGo met à jour un timestamp
`lastHeartbeatMs`. Si aucun heartbeat n'est reçu pendant un seuil configurable,
la LilyGo sait que la Waveshare est tombée (alerte, reboot, etc.).

**`SMS|id|number|text`** — Ordre d'envoi SMS. La LilyGo parse le paquet,
injecte le SMS dans sa queue existante (`sms_task`), et renvoie `ACK|id`
quand le modem a confirmé l'envoi.

### 7.3 Paquets envoyés par la LilyGo

**`STATE|canSms|ppp|rssi|bat`** — État périodique (toutes les ~5s), envoyé
en UDP unicast vers `192.168.4.10:5001`.

Champs :
- `canSms` : `1` si la LilyGo peut accepter un ordre SMS (PPP up, rate limit
  écoulé, place dans la queue), `0` sinon
- `ppp` : `1` si PPP connecté, `0` sinon
- `rssi` : signal cellulaire en dBm
- `bat` : pourcentage batterie (interpolation linéaire 3.0V=0%, 4.2V=100%)

**`ACK|id`** — Confirmation d'envoi SMS, envoyé après que le modem a réellement
transmis le SMS. L'id correspond à celui reçu dans le paquet `SMS|id|...`.

### 7.4 SMS propres LilyGo

La LilyGo envoie ses propres SMS (boot, watchdog) indépendamment de la
Waveshare. Ces SMS sont fire-and-forget, sans queue, avec un compteur
limité à 5 par boot. Le calcul batterie utilise l'interpolation linéaire
(3.0V=0%, 4.2V=100%) car `pmu.getBatteryPercent()` retourne des valeurs fausses.

---

## 8. SCÉNARIOS DE COORDINATION

### 8.1 La Waveshare publie normalement en MQTT

1. DataLogger::push() → MqttManager::onDataPushed() → publication MQTT
2. Si réussie → BridgeManager::onMqttPublish() → compteur++
3. Toutes les 5 publications → heartbeat UDP envoyé à la LilyGo
4. La LilyGo voit les heartbeats → la Waveshare fonctionne

### 8.2 La Waveshare veut envoyer un SMS

1. SmsManager appelle `BridgeManager::queueSms(number, message)`
2. BridgeManager vérifie `canAcceptSms` et `isReachable` dans le cache
3. Si OK → envoie `SMS|id|number|text` en UDP → passe en WAIT_ACK
4. La LilyGo injecte le SMS dans sa queue modem
5. Le modem envoie → la LilyGo renvoie `ACK|id`
6. BridgeManager reçoit l'ACK → succès, log, passe au SMS suivant

### 8.3 Le SMS n'arrive pas (ACK perdu ou modem échoue)

1. BridgeManager attend en WAIT_ACK (recvfrom non-bloquant à chaque handle)
2. Après 3 minutes sans ACK → vérifie `canAcceptSms`
3. Si oui → retry (2ème tentative)
4. Si non ou 2ème tentative → abandon + log
5. Le SMS est supprimé de la queue dans tous les cas

### 8.4 La LilyGo est injoignable

1. Pas de paquets STATE reçus depuis > 30s → `isReachable()` retourne false
2. Les SMS en queue ne sont pas envoyés (attente en IDLE)
3. Le heartbeat continue d'être envoyé (UDP fire-and-forget)
4. Quand la LilyGo revient → paquets STATE reprennent → `isReachable()` = true
5. Les SMS en queue sont envoyés

### 8.5 La Waveshare redémarre

1. Boot → WiFi STA → MQTT → NTP → stabilisation (~4 min)
2. BridgeManager démarre (socket UDP ouvert)
3. Premier paquet STATE reçu → cache mis à jour → LilyGo joignable
4. SMS de boot envoyé via SmsManager (si `DEBUG_SKIP_STARTUP_SMS = false`)

---

## 9. INFRASTRUCTURE DISTANTE — VPS + PWA

### 9.1 Vue d'ensemble

L'architecture distante repose sur trois composants indépendants :

```
[ ESP32 Waveshare ]
      │
      │ MQTT natif (esp_mqtt, port 8883)
      ▼
[ Broker HiveMQ Cloud ]  ←── MQTT WebSocket (port 8884) ──→  [ PWA téléphone(s) ]
      │                                                              │
      │ MQTT WebSocket                                               │
      ▼                                                              │
[ VPS Strato ]                                                       │
  Node.js + SQLite                                                   │
  API REST historique  ◄──────── HTTP GET ───────────────────────────┘
  Sert la PWA (HTML)
```

**Principe de séparation :**
- Le **temps réel** (données live + commandes) passe par MQTT directement
  entre la PWA et le broker. Le VPS n'est pas dans cette boucle.
- Le **stockage historique** est assuré par le VPS, qui écoute MQTT
  en observateur silencieux et expose les données via API REST.
- Le **schéma META** transite par MQTT (retain=true), disponible
  immédiatement pour tout client qui se connecte.

### 9.2 VPS — Micro-service Node.js

**Hébergement** : Strato VPS Linux (strato.nl)
- Plan VC 1-1 : 1 vCore, 1 GB RAM, 10 GB stockage
- Coût : 1€/mois + 9€ frais d'installation
- OS : Ubuntu 24.04 LTS
- Datacenter : Allemagne (EU, RGPD)

**Stack technique :**
- Node.js (runtime)
- mqtt.js (client MQTT — même bibliothèque que les prototypes de test)
- SQLite (base de données — fichier unique, zéro configuration)
- Express ou équivalent (routes REST)

**Le micro-service fait trois choses :**

**1. Écoute MQTT → stockage**
- Se connecte au broker HiveMQ en WebSocket (client ID : `serre-vps`)
- S'abonne à `serre/data/#`, `serre/status/#`, `serre/bridge/#`, `serre/schema`
- À chaque message données, parse le payload CSV et insère dans SQLite
- À la réception d'un schéma, compare le hash CRC32 avec le cache local
  et met à jour si nécessaire
- **Validation avant insertion** (protection contre payloads corrompus) :
  - Nombre de champs = 5 (après parsing CSV respectant les guillemets)
  - `type` ∈ {0, 1, 2, 3}
  - `id` ∈ {0 .. Count-1} (Count connu via le schéma)
  - `valueType` ∈ {0, 1}
  - `timestamp` raisonnable : pas dans le futur (+ marge 60s),
    pas antérieur au 1er janvier 2024
  - Les lignes invalides sont logguées (topic, payload brut) et ignorées

**2. API REST pour l'historique**
- Dernières valeurs de chaque capteur (état actuel au chargement)
- Historique d'un capteur sur une période (courbes)
- Agrégations (moyennes horaires/journalières pour les tendances)
- Le schéma JSON courant (pour les consommateurs qui ne font pas de MQTT)

**3. Servir la PWA**
- Le fichier HTML de la PWA + manifest + service worker
- Servi sur la route racine du VPS
- Ne sert qu'au premier chargement et aux mises à jour

### 9.3 Stockage SQLite

**Initialisation :**
```sql
PRAGMA journal_mode=WAL;    -- Write-Ahead Logging : améliore les lectures
                            -- concurrentes (PWA lit pendant que le service écrit)
```

**Table principale :**

```sql
CREATE TABLE measures (
    timestamp INTEGER NOT NULL,   -- UTC secondes (horodatage Waveshare)
    type      INTEGER NOT NULL,   -- DataType (0-3)
    id        INTEGER NOT NULL,   -- DataId (0 à Count-1)
    valueType INTEGER NOT NULL,   -- 0=float, 1=texte
    value     TEXT NOT NULL        -- Valeur brute
);

CREATE INDEX idx_measures_id_ts ON measures(id, timestamp);
```

**Volume estimé :**
- ~20 valeurs/heure × 24h × 365j = ~175 000 lignes/an
- ~50 octets/ligne = ~9 MB/an
- Sur 14 mois = ~205 000 lignes, ~10 MB
- SQLite gère sans difficulté des bases de plusieurs gigaoctets

**Batching des écritures :**
Les publications MQTT arrivent par rafales (plusieurs `DataLogger::push()`
dans la même tâche TaskManager). Le micro-service accumule les messages reçus
dans un buffer en mémoire et les insère en une seule transaction SQLite
toutes les ~10 secondes. Une transaction groupée avec N INSERT est beaucoup
plus efficace que N transactions individuelles (chaque transaction implique
un flush disque).

**Rétention FIFO 14 mois :**
- Purge quotidienne automatique (timer dans le micro-service)
- `DELETE FROM measures WHERE timestamp < strftime('%s', 'now', '-14 months')`
- Permet la comparaison d'une année sur l'autre (12 mois + 2 mois de marge)

**Maintenance :**
- `VACUUM` mensuel automatique pour récupérer l'espace disque libéré par
  les purges quotidiennes (SQLite ne libère pas l'espace automatiquement
  après un DELETE)
- Monitoring de la taille du fichier de base (log périodique)

**Table schéma :**

```sql
CREATE TABLE schema (
    hash      TEXT PRIMARY KEY,   -- CRC32 hex
    json      TEXT NOT NULL,      -- Schéma JSON complet
    received  TEXT NOT NULL       -- Date de réception ISO 8601
);
```

### 9.4 PWA (Progressive Web App)

**Nature :** fichier HTML unique avec CSS et JS intégrés (même approche
que les prototypes de test existants). Installable sur Android comme
une app via le manifest PWA.

**Fonctionnement :**
- Se connecte à `wss://...hivemq.cloud:8884/mqtt` via mqtt.js
- Client ID : `serre-pwa-{random}` (unique par instance)
- S'abonne à `serre/data/#` pour le temps réel
- S'abonne à `serre/status/#` pour les indicateurs online/offline
- S'abonne à `serre/schema` pour recevoir le schéma META (retain=true)
- Publie sur `serre/cmd/{action}` pour les commandes (arrosage, etc.)
- Interroge l'API REST du VPS pour l'historique et les statistiques

**Parser unique :** le même code JavaScript parse les payloads MQTT
(temps réel) et les réponses REST (historique). Le format CSV
`timestamp,type,id,valueType,value` est identique dans les deux cas.

**Interprétation des données :** le schéma reçu sur `serre/schema`
fournit les labels, unités, nature (métrique/état/texte) et libellés
d'états. La PWA n'a aucune connaissance codée en dur des DataId.

**Design :** dark theme validé (DM Sans/DM Mono, fond #0e1117, cartes
avec bordure #2a313a) — continuité avec les prototypes existants.

### 9.5 Double accès

| Mode | Chemin | Interface | Données |
|------|--------|-----------|---------|
| **Local** | WiFi AP Waveshare → `http://192.168.5.1` | Site web embarqué (HTML généré C++) | Temps réel + historique SPIFFS |
| **Distant** | GSM/4G/WiFi maison → PWA | PWA hébergée sur VPS | Temps réel MQTT + historique VPS |

Les deux interfaces sont indépendantes. Le site embarqué n'a pas besoin
du VPS ni de MQTT. La PWA n'a pas besoin du WiFi AP de la Waveshare.

### 9.6 Robustesse et modes dégradés

| Panne | Temps réel | Commandes | Historique |
|-------|-----------|-----------|------------|
| VPS tombe | ✅ fonctionne (MQTT direct) | ✅ fonctionne (MQTT direct) | ❌ indisponible |
| HiveMQ tombe | ❌ pas de données distantes | ❌ pas de commandes distantes | ✅ historique existant accessible |
| LilyGo tombe | ❌ pas d'internet | ❌ pas de commandes distantes | ✅ historique existant accessible |
| Waveshare tombe | ❌ pas de données | ❌ pas de commandes | ✅ historique existant accessible |

**Point clé :** la PWA est en cache sur le téléphone (service worker).
Même si le VPS est injoignable, la PWA se lance depuis le cache local.
Le temps réel et les commandes passent par HiveMQ, pas par le VPS.
Le VPS n'est jamais un point critique pour l'usage quotidien.

### 9.7 Sécurité

Credentials MQTT visibles dans le code de la PWA et du micro-service VPS.
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

Fréquence : toutes les 5 minutes (tâche dédiée ou ajouté à une tâche
périodique existante comme la tâche WiFi status).

**Seuil d'alerte** : si le minimum free descend sous ~50 KB, investiguer.
Les connexions TLS consomment ~40-50 KB temporairement pendant le handshake.

---

## 11. PLAN D'IMPLÉMENTATION

### Phase 1 — BridgeManager + MqttManager + SmsManager Waveshare — **✅ FAIT (v4.3)**
1. Créer BridgeManager.h/.cpp : **✅ FAIT**
   - Transport UDP unicast (§5.2)
   - Cache état LilyGo passif (§5.4)
   - Queue SMS 2 slots + machine d'états IDLE/WAIT_ACK (§5.6)
   - Heartbeat couplé MQTT via callback (§5.5)
   - Démarrage différé 4 minutes (§5.7)
   - Logging Console + DataLogger pour tous les événements SMS
2. Créer MqttManager.h/.cpp : **✅ FAIT (v4.2)**
   - Architecture réactive via callback `onDataPushed()` (§6.2)
   - Démarrage conditionnel `ensureMqttStarted()` (WiFi STA)
   - Publication du schéma au boot (§4.5)
   - Payload format CSV (§4.4)
   - Callback `setOnPublishSuccess()` pour heartbeat (v4.3)
3. Refactorer SmsManager : **✅ FAIT (v4.3)**
   - Logique métier uniquement (SMS de boot, alertes)
   - Transport délégué à `BridgeManager::queueSms()`
   - Flag `DEBUG_SKIP_STARTUP_SMS` conservé
4. Ajouter les defines dans NetworkConfig.h et TimingConfig.h **✅ FAIT**
5. Intégrer dans main.cpp (init + callbacks + tâches) **✅ FAIT**
6. Tester la connexion TLS vers HiveMQ **✅ FAIT — fonctionnel**
7. Tester le démarrage différé et l'impact NTP **✅ FAIT — fonctionnel**

### Phase 2 — Protocole UDP côté LilyGo
1. Ajouter tâche FreeRTOS UDP listener (port 5000)
2. Parser les paquets HB et SMS
3. Injecter les SMS reçus dans la queue modem existante (`sms_task`)
4. Envoi périodique STATE (~5s) vers 192.168.4.10:5001
5. Envoi ACK après confirmation modem
6. SMS propres LilyGo au boot (batterie, signe de vie)
7. Tester la chaîne complète : SmsManager → BridgeManager → UDP → LilyGo → modem

### Phase 3 — Publication des données — **✅ FAIT (v4.2)**
1. Publication immédiate via callback `onDataPushed()` à chaque `DataLogger::push()`
2. LWT retain + "online" explicite
3. Publication du schéma JSON au boot (retain=true, QoS 1)
4. Heartbeat couplé MQTT — **✅ FAIT (v4.3)**

### Phase 4 — Réception commandes
1. Abonnement aux topics `serre/cmd/#` **✅ FAIT**
2. Callback → queue FreeRTOS → mécanisme de dépilage à définir
3. Dispatch vers le module d'arrosage (futur)

### Phase 5 — VPS + Micro-service
1. Provisionner le VPS Strato (VC 1-1, Ubuntu 24.04)
2. Installer Node.js, créer le micro-service
3. Connexion MQTT au broker (mqtt.js, client ID `serre-vps`)
4. Créer la base SQLite, table measures + index
5. Implémenter l'écoute MQTT → INSERT SQLite
6. Implémenter la purge FIFO 14 mois (timer quotidien)
7. Implémenter les routes REST (dernières valeurs, historique, agrégations)
8. Implémenter la réception et le cache du schéma
9. Tester la chaîne complète : ESP32 → HiveMQ → VPS → SQLite → REST

### Phase 6 — PWA téléphone
1. Page HTML unique, mqtt.js, design dark existant
2. Réception du schéma via `serre/schema` (retain)
3. Cartes dynamiques pour chaque DataId (générées depuis le schéma)
4. Temps réel via MQTT WebSocket
5. Boutons de commande vannes (publish `serre/cmd/...`)
6. Indicateur online/offline via LWT retain
7. Graphiques historiques via API REST du VPS
8. Manifest PWA + service worker pour installation sur Android
9. Héberger la PWA sur le VPS

### Phase 7 — MQTT sur LilyGo (futur)
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
- `WebServer.cpp` — serveur web, routes, bundle download, schéma JSON auto-généré
- `PagePrincipale.cpp` — dashboard HTML, intégration DataLogger
- `PageLogs.cpp` — gestion des logs, téléchargement bundle
- `DataLogger.h` — table META (source de vérité), enums DataType/DataId/DataNature
- `DataLogger.cpp` — push, flush CSV, format `timestamp,type,id,valueType,value`
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
8. **Un seul format de données** — CSV identique entre SPIFFS, MQTT et VPS
9. **Une seule source de vérité** — la table META dans DataLogger.h génère
   tout : schéma JSON, affichage web embarqué, schéma MQTT auto-publié
10. **Pas de HTTP vers la LilyGo** — le transport Waveshare ↔ LilyGo est
    exclusivement UDP (le HTTP bloque le TaskManager sur radio partagée AP+STA)

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

### v4 (17 mars 2026)
- **Payload MQTT** : format CSV identique au SPIFFS
- **Schéma auto-publié** : topic `serre/schema` (retain=true) avec hash CRC32
- **VPS Strato** : micro-service Node.js + SQLite
- **PWA** : architecture complète définie
- **Robustesse** : analyse des modes dégradés
- **Stockage** : FIFO circulaire 14 mois dans SQLite
- **Client IDs** : ajout `serre-vps` et `serre-pwa-{random}`
- **Règles de développement** : ajout règles 8 et 9

### v4.1 (17 mars 2026)
- Spécification stricte du format CSV (contrat d'encodage)
- Validation des payloads côté VPS
- SQLite WAL + batching
- VACUUM mensuel

### v4.1 (18 mars 2026)
- Suppression de l'émission du schéma des datas en dehors du reboot

### v4.2 (26 mars 2026)
Mise à jour après implémentation réelle de MqttManager sur la Waveshare.
Le design théorique (machine d'états, tâches périodiques) a été remplacé
par un design réactif plus simple, validé en fonctionnement :

- **MqttManager réactif** (§6.2) : plus de machine d'états, plus de
  `handle()`. Déclenché par le callback `onDataPushed()` de DataLogger.
- **Démarrage conditionnel WiFi STA** (§6.2) : `init()` configure,
  `ensureMqttStarted()` démarre au premier push après connexion WiFi STA.
- **Aucune tâche TaskManager** (§6.4) : le rythme de publication est
  dicté par les capteurs et le DataLogger, pas par MqttManager.
- **Publication immédiate** (§6.5) : chaque `DataLogger::push()` publie
  immédiatement si MQTT est connecté.
- **Nommage explicite** : `mqttStarted`, `mqttConnected`, `isMqttConnected()`.

### v4.3 (28 mars 2026)
Implémentation de BridgeManager et refactoring SmsManager. **Abandon du
transport HTTP** vers la LilyGo au profit d'UDP unicast, suite à l'échec
constaté du HTTP sur radio partagée AP+STA (blocage >300ms, fuites de sockets).

- **BridgeManager implémenté** (§5) : module complet de communication
  Waveshare ↔ LilyGo via UDP unicast. Cache état LilyGo passif (paquets STATE),
  queue SMS 2 slots avec ACK + retry, heartbeat couplé MQTT. Tourne dans
  le TaskManager (500ms), 100% non-bloquant.
- **Transport UDP** (§5.2) : remplacement de HTTP par UDP. Pas de handshake
  TCP, pas de fuite de sockets. Un `sendto()` prend quelques ms, un `recvfrom()`
  non-bloquant retourne immédiatement. Protocole pipe-séparé : HB, SMS|id|num|txt,
  STATE|canSms|ppp|rssi|bat, ACK|id.
- **Démarrage différé 4 minutes** (§5.7) : le socket UDP n'est ouvert qu'après
  stabilisation complète (WiFi STA, MQTT, NTP). Résout le problème de timeouts
  NTP constaté quand le socket était ouvert au boot.
- **Heartbeat couplé MQTT** (§5.5) : envoyé après 5 publications MQTT réussies.
  Chaîne de callbacks : DataLogger → MqttManager → BridgeManager. Signifie
  "la Waveshare est vivante ET produit des données".
- **MqttManager enrichi** (§6.2) : ajout `setOnPublishSuccess()` — callback
  appelé après chaque publication réussie. Même pattern que `DataLogger::setOnPush()`.
- **SmsManager refactoré** : réduit à la logique métier (quand/quoi envoyer).
  Transport délégué à `BridgeManager::queueSms()`. Plus de HTTPClient, ArduinoJson,
  machine d'états HTTP, ni polling de statut.
- **Section LilyGo réécrite** (§7) : remplacement de l'endpoint HTTP par le
  protocole UDP (listener, STATE périodique, ACK).
- **Scénarios réécrits** (§8) : adaptés au modèle UDP.
- **Suppression tâche FreeRTOS BridgeManager** (§1.6) : UDP étant non-bloquant,
  plus besoin de tâche dédiée.
- **Règle 10 ajoutée** (§13) : pas de HTTP vers la LilyGo.
