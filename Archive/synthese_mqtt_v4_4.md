# SYNTHÈSE ARCHITECTURE MQTT — SERRE CONNECTÉE
## Document de référence — Version 4.4

**Date** : 29 mars 2026
**Statut** : Phase 1 et Phase 2 implémentées et validées

---

## 1. MATÉRIEL

Deux cartes ESP32-S3 collaborent pour piloter une serre :

**Waveshare ESP32-S3-Relay-6CH** (carte principale)
- Rôle : acquisition capteurs, commande relais/vannes, serveur web local, publication MQTT
- WiFi AP+STA simultané : AP `192.168.5.1` (accès local), STA `192.168.4.10` (vers LilyGo)
- Framework : Arduino (PlatformIO)
- Architecture non-bloquante, orchestrée par TaskManager (callbacks < 50ms)
- RTC DS3231 intégrée (horodatage fiable dès le boot)

**LilyGo T-SIM7080G-S3** (pont WiFi-GSM)
- Rôle : pont NAT WiFi → Cat-M (PPPoS) vers internet + service SMS + communication UDP
- WiFi SoftAP `192.168.4.1`, SSID `Pont_Wifi-GSM_de_la_serre`, 1 client max
- Modem SIM7080G, APN `domotec82.fr`
- PMU AXP2101 (batterie intégrée)
- Framework : ESP-IDF pur (FreeRTOS)
- Firmware v9.0-udp-bridge

**Architecture réseau :**
```
Internet (Cat-M)
      |
  [ LilyGo 192.168.4.1 ]  SoftAP + NAT/NAPT
      |
  [ Waveshare 192.168.4.10 ]  STA + AP 192.168.5.1
      |
  [ Téléphone / PC ]
```


## 2. ARBORESCENCE WAVESHARE

```
src/
├── main.cpp
├── Config/
│   ├── Config.h
│   ├── IO-Config.h
│   ├── NetworkConfig.h
│   └── TimingConfig.h
├── Connectivity/
│   ├── WiFiManager.h/.cpp      — Machine d'états WiFi AP+STA
│   ├── ManagerUTC.h/.cpp       — Maître du temps + Agent NTP
│   ├── BridgeManager.h/.cpp    — Communication UDP vers LilyGo
│   ├── MqttManager.h/.cpp      — Client MQTT réactif (esp_mqtt)
│   └── SmsManager.h/.cpp       — Logique métier SMS
├── Core/
│   ├── TaskManager.h/.cpp      — Tâches périodiques
│   ├── EventManager.h/.cpp     — Observation états
│   ├── RTCManager.h/.cpp       — RTC DS3231
│   ├── VirtualClock.h/.cpp     — Horloge virtuelle
│   └── SafeReboot.h/.cpp       — Reboot préventif mensuel
├── Sensors/
│   └── DataAcquisition.h/.cpp
├── Storage/
│   └── DataLogger.h/.cpp       — Source de vérité unique (table META)
├── Utils/
│   └── Console.h/.cpp
└── Web/
    ├── WebServer.h/.cpp
    └── Pages/
```


## 3. BROKER MQTT — HIVEMQ CLOUD

| Paramètre | Valeur |
|-----------|--------|
| URL MQTTS (ESP32) | `mqtts://3db6155980d4483e8b8c3036fd0afd6f.s1.eu.hivemq.cloud:8883` |
| URL WebSocket (PWA) | `wss://...hivemq.cloud:8884/mqtt` |
| Username | `Graindesable` |
| Password | `Chaperonrouge64` |
| TLS | Obligatoire (ISRG Root X1) |
| Plan | Serverless gratuit |


## 4. TOPICS MQTT

**Données Waveshare → Broker :**
```
serre/data/{id}             — Un topic par DataId (QoS 0)
serre/status/waveshare      — LWT "offline" / "online" (retain, QoS 1)
serre/schema                — Schéma JSON META (retain, QoS 1, au boot)
```

**Commandes → Waveshare :**
```
serre/cmd/{action}          — Abonnement en place, dispatch futur (Phase 4)
```

**Client IDs :**
| Carte / Service | Client ID |
|----------------|-----------|
| Waveshare | `serre-waveshare` |
| VPS (futur) | `serre-vps` |
| PWA (futur) | `serre-pwa-{random}` |


## 5. FORMAT DONNÉES

**Payload CSV 7 champs** (identique SPIFFS, MQTT, VPS) :
```
timestamp,UTC_available,UTC_reliable,type,id,valueType,value
```

Exemples :
```
1710684600,1,1,1,1,0,23.500     — Température 23.5°C
1710684600,1,1,3,8,1,"Démarrage" — Boot texte
```

Contrat d'encodage : séparateur virgule, UTF-8, guillemets RFC 4180
pour valueType=1, pas de split naïf sur virgule.


## 6. MQTTMANAGER (Waveshare)

Architecture purement **réactive** — pas de machine d'états, pas de handle(),
pas de tâche TaskManager.

**Mécanisme :**
- `init()` configure esp_mqtt mais ne démarre pas la connexion
- `ensureMqttStarted()` fait le `start()` au premier push après WiFi STA
- `onDataPushed()` publie immédiatement si MQTT est connecté
- esp_mqtt gère sa propre tâche FreeRTOS (TLS, keepalive 90s, reconnexion)

**Callback publication réussie :**
- `setOnPublishSuccess(callback)` — notifie BridgeManager pour le heartbeat
- Même pattern que `DataLogger::setOnPush()`

**Chaîne de callbacks dans main.cpp :**
```
DataLogger::setOnPush(MqttManager::onDataPushed);
MqttManager::setOnPublishSuccess(BridgeManager::onMqttPublish);
```


## 7. BRIDGEMANAGER (Waveshare)

Point unique de communication Waveshare ↔ LilyGo via UDP unicast.
Ni MqttManager, ni SmsManager ne communiquent directement avec la LilyGo.

### 7.1 Transport UDP

```
Waveshare (192.168.4.10:5001) ←─UDP─→ LilyGo (192.168.4.1:5000)
```

Ports définis dans NetworkConfig.h :
```cpp
static constexpr uint16_t BRIDGE_UDP_PORT_LOCAL  = 5001;
static constexpr uint16_t BRIDGE_UDP_PORT_REMOTE = 5000;
```

### 7.2 Protocole (4 paquets, format pipe-séparé, sans identifiant)

| Direction | Paquet | Signification |
|-----------|--------|---------------|
| Waveshare → LilyGo | `HB` | Heartbeat (signe de vie) |
| Waveshare → LilyGo | `SMS\|number\|text` | Ordre d'envoi SMS |
| LilyGo → Waveshare | `STATE\|0` ou `STATE\|1` | Disponibilité SMS (~30s) |
| LilyGo → Waveshare | `ACK` | SMS envoyé par le modem |

### 7.3 État LilyGo

Un seul booléen `canAcceptSms`, mis à jour passivement par les paquets STATE.
Accesseur public : `BridgeManager::canAcceptSms()`.

### 7.4 Heartbeat

Couplé au métier MQTT : envoyé après 5 publications MQTT réussies.
Signifie "la Waveshare est vivante ET produit des données".
Si le métier s'arrête, le heartbeat s'arrête — c'est l'alerte pour la LilyGo.

Chaîne : DataLogger → MqttManager → BridgeManager::onMqttPublish() → compteur → HB.

### 7.5 Cycle SMS

Queue de 2 slots fixes. Si pleine, le nouveau SMS est rejeté (pas d'écrasement).

Machine d'états :
```
IDLE
  └─ SMS en queue + canAcceptSms → envoi UDP → WAIT_ACK

WAIT_ACK
  ├─ ACK reçu → succès, log, supprime SMS → IDLE
  └─ Timeout 3 min :
       ├─ tentative 1 + canAcceptSms → retry → WAIT_ACK
       └─ sinon → abandon + log → IDLE
```

Chaque événement SMS est loggé Console + DataLogger::push(DataId::Error)
→ publication MQTT automatique.

### 7.6 Démarrage différé

Le socket UDP n'est ouvert qu'après 4 minutes (`BRIDGE_START_DELAY_MS`).
Pendant ce délai, `handle()` retourne immédiatement.
Raison : un socket ouvert au boot perturbait le NTP (timeouts répétés
sur la radio partagée AP+STA).

### 7.7 Timings (TimingConfig.h)

```cpp
#define BRIDGE_START_DELAY_MS       240000UL    // 4 min (démarrage différé)
#define BRIDGE_HANDLE_PERIOD_MS     500         // Période handle() TaskManager
#define BRIDGE_SMS_ACK_TIMEOUT_MS   180000UL    // 3 min (timeout ACK)
```

### 7.8 Intégration main.cpp

```cpp
BridgeManager::init();          // Après SafeReboot, avant MqttManager
MqttManager::init();
DataLogger::setOnPush(MqttManager::onDataPushed);
MqttManager::setOnPublishSuccess(BridgeManager::onMqttPublish);
SmsManager::init();

TaskManager::addTask([]() { BridgeManager::handle(); }, BRIDGE_HANDLE_PERIOD_MS);
TaskManager::addTask([]() { SmsManager::handle(); }, 2000);
```


## 8. SMSMANAGER (Waveshare)

Logique métier SMS uniquement. Transport délégué à BridgeManager.

- SMS de bienvenue au boot (après stabilisation WiFi STA, ~60s)
- Flag `DEBUG_SKIP_STARTUP_SMS` en haut du fichier pour désactiver en dev
- `alert(message)` envoie à tous les numéros de la whitelist
- `send(number, message)` appelle `BridgeManager::queueSms()`
- Plus de HTTPClient, ArduinoJson, machine d'états HTTP, ni polling


## 9. LILYGO — FIRMWARE v9.0-udp-bridge

### 9.1 Séquence de boot (13 étapes)

1. NVS + netif + event loop
2. PMU AXP2101
3. GPIO modem
4. PPPoS → attente IP
5. WiFi SoftAP
6. NAPT (DNS + NAT)
7. Mutex modem
8. Queue + Mutex + Table SMS
9. Serveur HTTP — **DÉSACTIVÉ** (code conservé en `#if 0`)
10. Tâche SMS (prio 5)
11. Tâche reconnexion PPP (prio 4)
12. Tâche UDP Bridge (prio 3)
13. SMS de boot

### 9.2 Tâche UDP Bridge

Écoute UDP port 5000. Boucle infinie :
- `recvfrom` avec timeout 1s (réception fiable, latence sans importance)
- Dispatch `HB` → met à jour `last_activity_ms` (watchdog)
- Dispatch `SMS|number|text` → validation + injection queue
- Envoi `STATE|0` ou `STATE|1` toutes les 30s

### 9.3 Fonction can_accept_sms()

Détermine si la LilyGo peut accepter un ordre SMS :
```
PPP connecté
&& place dans la queue
&& rate limit écoulé (5 min entre deux envois, réussis ou pas)
&& pas en train de traiter un SMS (sms_processing == false)
```

Pas de mutex modem dans ce calcul.

### 9.4 Réception SMS|number|text

1. Vérifier `can_accept_sms()` → si false, ignorer silencieusement
2. Valider le numéro : `sms_validate_number_format()` + `sms_check_whitelist()`
3. Valider le texte : non vide, ≤ 160 caractères
4. Si validation échoue → ignorer silencieusement (log local uniquement)
5. Créer l'entrée dans `sms_table` avec `from_udp = true`
6. Envoyer dans `sms_queue`

La Waveshare gère l'absence d'ACK via son cycle timeout 3 min / retry / abandon.

### 9.5 Envoi ACK dans sms_task

Après `sms_table[idx].status = SMS_STATUS_SENT` :
```cpp
if (sms_table[idx].from_udp) {
    send_udp_ack();             // Envoie "ACK" en UDP
    sms_table[idx].from_udp = false;
}
```

L'ACK n'est envoyé que quand le modem a confirmé l'envoi (`esp_modem_send_sms()` OK).
Si échec ou timeout modem (~120s), pas d'ACK.

### 9.6 Flag sms_processing

`sms_processing = true` au début du traitement, `= false` à chaque sortie.
Utilisé par `can_accept_sms()` pour refléter que la LilyGo est occupée
(bascule modem, envoi, attente confirmation réseau).

### 9.7 Rate limit

Mis à jour sur **toute tentative** d'envoi (réussie ou pas), pas seulement
sur succès. `last_sms_sent_ms` est écrit avant le test `send_success`.

### 9.8 SMS de boot LilyGo

Envoyé à chaque numéro de la whitelist après l'étape 13 :
```
Reseau de la serre de Marie-Pierre actif (MPFE) - Batterie 96%
```

Pourcentage batterie par interpolation linéaire :
`pct = (pmu.getBattVoltage() - 3000) * 100 / (4200 - 3000)`, clamped 0-100.

Injection dans `sms_table` + `sms_queue` avec `from_udp = false` (pas d'ACK).

### 9.9 Watchdog LilyGo

Reboot si aucun heartbeat UDP pendant 2h ET PPP down.
Le heartbeat met à jour `last_activity_ms` sous spinlock.
C'est la seule condition de reboot du système.

### 9.10 Serveur HTTP

Tout le code HTTP (handlers SMS + heartbeat + serveur) est conservé
en commentaire (`#if 0`) pour réactivation future.
Les fonctions `sms_table_find`, `sms_send_json_response`, `sms_post_handler`,
`sms_get_handler`, `sms_options_handler`, `heartbeat_get_handler`,
`http_server_init` sont toutes commentées.


## 10. SÉQUENCE DE BOOT COMPLÈTE

**LilyGo (démarrée en premier) :**
1. Boot modem + PPP (~36s)
2. WiFi SoftAP + NAPT
3. Tâches SMS, PPP reconnexion, UDP Bridge
4. SMS de boot envoyé (modem ~400ms)
5. STATE|0 toutes les 30s (rate limit actif 5 min)
6. STATE|1 après ~5 min

**Waveshare (démarrée après) :**
1. Boot + WiFi AP (~9s)
2. WiFi STA connecté à LilyGo (~70s, variable)
3. MQTT connecté au broker (~93s, TLS ~3s)
4. NTP synchro (~190s, délai 2 min après WiFi)
5. BridgeManager démarré (~249s, délai 4 min)
6. Heartbeats UDP réguliers (~30s d'intervalle)
7. Réception STATE → `canAcceptSms` mis à jour


## 11. INFRASTRUCTURE DISTANTE (FUTUR)

### VPS Strato (Phase 5)
- Node.js + mqtt.js + SQLite + Express
- Écoute MQTT silencieuse → stockage historique
- API REST pour la PWA
- Rétention FIFO 14 mois, purge quotidienne, VACUUM mensuel

### PWA téléphone (Phase 6)
- HTML unique + mqtt.js, dark theme
- Temps réel via MQTT WebSocket direct (pas via VPS)
- Historique via API REST du VPS
- Schéma dynamique via `serre/schema` (retain)
- Installable sur Android (service worker)


## 12. PLAN D'IMPLÉMENTATION

| Phase | Contenu | Statut |
|-------|---------|--------|
| 1 | BridgeManager + MqttManager + SmsManager Waveshare | ✅ Fait |
| 2 | Protocole UDP côté LilyGo | ✅ Fait |
| 3 | Publication données MQTT | ✅ Fait |
| 4 | Réception commandes (serre/cmd/#) | À faire |
| 5 | VPS + Micro-service Node.js | À faire |
| 6 | PWA téléphone | À faire |
| 7 | MQTT sur LilyGo (serre/bridge/...) | Futur |


## 13. RÈGLES DE DÉVELOPPEMENT

1. Ne jamais coder avant accord sur le design
2. Ne jamais modifier de code en dehors du périmètre demandé
3. Toute hypothèse expliquée et validée avant implémentation
4. Il est possible de dire "je ne sais pas"
5. Il est possible de se tromper — on le dit et on rectifie
6. Aspect non-bloquant sacré — le TaskManager ne doit jamais être bloqué
7. Stabilité avant fonctionnalités
8. Un seul format de données — CSV identique SPIFFS / MQTT / VPS
9. Une seule source de vérité — table META dans DataLogger.h
10. Pas de HTTP vers la LilyGo — transport exclusivement UDP


## 14. PROBLÈME OUVERT

**Erreurs CCMP** : des messages `wifi:CCMP mgmt frame from ... used non-zero reserved bit`
apparaissent côté LilyGo quand la Waveshare est connectée en WiFi STA.
Ces erreurs sont apparues pendant le développement de BridgeManager.
Le trafic fonctionne normalement (heartbeats, STATE, MQTT) mais les logs
sont pollués. Investigation en cours dans un chat séparé.
