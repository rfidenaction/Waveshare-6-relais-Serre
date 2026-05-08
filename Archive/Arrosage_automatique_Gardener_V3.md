# Conception du module Gardener V3 — Système de gestion de serre



## Contexte et objectif

Gardener est le programmateur d'arrosage temporel d'un système de gestion de serre. Il s'intègre dans un firmware ESP32-S3 existant (carte Waveshare ESP32-S3-Relay-6CH) qui pilote 6 électrovannes via ValveManager. L'UI (web et MQTT) permet déjà des arrosages manuels. On ajoute maintenant des arrosages programmés.

## Périmètre fonctionnel

Arrosage temporel (en prévoyant une condition d'hygrométrie dès l'origine). L'utilisateur définit pour chacune des 6 vannes jusqu'à 6 créneaux indépendants par jour, identiques tous les jours. Un créneau = une heure de début (heure locale) + une durée. Les créneaux sont créés/supprimés à la volée par l'utilisateur via MQTT (et plus tard via WebServer), et persistent à travers les reboots.

Pas de modification d'un créneau : pour changer un créneau existant, l'utilisateur le supprime et en crée un nouveau.

Capacité totale : 6 vannes × 6 créneaux = 36 créneaux maximum.

### Hors scope

- Logique d'arrosage conditionnel par capteur d'hygrométrie (le champ correspondant est néanmoins prévu dans la structure de données et le format MQTT/JSON).
- Module de fiabilisation des données capteurs (sujet à traiter dans un autre chat dédié).

---

## Architecture du module

### Emplacement

Le nom « Gardener » incarne le métier jardinier et permet l'extension future à d'autres actionneurs (volets, ventilateurs, éclairage) sans renommage du module.

- Répertoire : `src/Gardener/`
- Fichiers : `GardenerManager.h` et `GardenerManager.cpp`
- Classe statique : `GardenerManager` (même pattern que ValveManager, MqttManager, DataLogger)
- Tag console : `"Gardener"`

### Tâche périodique

Conformément à la convention projet, le timing des tâches périodiques est défini dans `Config/TimingConfig.h`.

`GardenerManager::handle()` enregistrée dans TaskManager avec période 1000 ms (constante `GARDENER_HANDLE_PERIOD_MS` dans `TimingConfig.h`). Période choisie pour ne jamais rater une transition de minute. Pas de tâche FreeRTOS dédiée — cohérent avec ValveManager. TaskManager est supervisé par TaskManagerMonitor.

---

## Nommage — convention validée

Tout porte le préfixe `Gardener`et/ou le mot `Watering` pour expliciter l'arrosage automatique.  
  
Exemples :

| Élément | Nom |
|---|---|
| Classe | `GardenerManager` |
| Struct créneau | `GardenerWateringSlot` |
| Tableau de créneaux | `gardenerWateringSlots[]` |
| Compteur | `gardenerWateringSlotCount` |
| Variable de minute | `gardenerLastMinute` |
| Fichier flash | `/gardener.json` |
| Fichier temporaire (écriture atomique) | `/gardener.tmp` |
| Constantes topics | `GARDENER_TOPIC_FROM_USER` / `GARDENER_TOPIC_TO_USER` |
| Réception MQTT | `GardenerManager::onGardenerMessage()` |
| Publication état (GardenerManager) | `GardenerManager::publishGardenerWateringState()` |
| Publication MQTT (MqttManager) | `MqttManager::publishGardenerWateringState()` |
| Constante timing | `GARDENER_HANDLE_PERIOD_MS` |
| Add/remove internes | `addGardenerWateringSlot()` / `removeGardenerWateringSlot()` |
| Validation | `validateGardenerWateringSlot()` |
| Persistance | `saveGardenerWateringSlots()` / `loadGardenerWateringSlots()` |
| Sérialisation JSON | `serializeGardenerWateringSlots()` |

---

## Structure de données

La structure des données se trouve dans Config/MetaDataModel.h qui est la source unique de vérité.  
Tout changement dans ce référent doit se répercuter automatiquement dans le reste du code.  
Concernant les relais/vannes/commandes le tableau de référence est IO-Config qui sert également de source unique de vérité.

### GardenerWateringSlot (Programmes)

```cpp
struct GardenerWateringSlot {
    DataId   cmdId;               // CommandValve1..CommandValve6 (ids 17..22)
    uint8_t  hour;                // 0..23 (heure locale)
    uint8_t  minute;              // 0..59
    uint16_t duration;            // secondes, 1..900
    bool     cancellableBySensor; // toujours false en v1, ignoré au déclenchement
};
```

### Constantes (exemples)

```cpp
static constexpr uint8_t  MAX_WATERING_SLOTS_PER_VALVE = 6;
static constexpr uint8_t  MAX_WATERING_SLOTS_TOTAL     = 36;  // 6 × 6
static constexpr const char* GARDENER_TOPIC_FROM_USER  = "serre/gardener/FromUser";
static constexpr const char* GARDENER_TOPIC_TO_USER    = "serre/gardener/ToUser";
```

### Données statiques (exemples)

| Variable | Type | Init | Rôle |
|---|---|---|---|
| `gardenerWateringSlots[]` | `GardenerWateringSlot[36]` | vide | Tableau fixe, pas d'allocation dynamique |
| `gardenerWateringSlotCount` | `uint8_t` | 0 | Nombre d'entrées valides |
| `gardenerLastMinute` | `uint16_t` | `0xFFFF` | Dernière minute du jour traitée (0xFFFF = jamais) |

### Remarques sur les champs

- `hour` et `minute` sont en **heure locale** (ce que l'utilisateur saisit, mais aussi ce que l'UI affiche).
- Tout ce qui est traité en interne et stocké (timestamps DataRecord, CSV, VirtualClock) reste en **UTC**.
- La zone de référence de conversion se trouve dans Config/Config.h
- Le champ `cancellableBySensor` est présent dès la v1 dans la structure, dans le JSON persisté et dans les payloads MQTT. Il est toujours initialisé à false et ignoré au déclenchement. Quand le module de fiabilité capteur sera disponible, on n'aura qu'un seul endroit à modifier dans GardenerManager.

---

## Structure de la classe GardenerManager (exemple)

```
GardenerManager (classe statique)
├── public
│   ├── init()                                 — charge /gardener.json, log le nombre de créneaux
│   ├── handle()                               — tâche périodique 1000 ms
│   ├── onGardenerMessage(data, len)           — appelée par MqttManager (thread esp_mqtt)
│   └── publishGardenerWateringState()         — construit JSON, appelle MqttManager
│
└── private
    ├── addGardenerWateringSlot(slot)           — valide + ajoute + sauvegarde
    ├── removeGardenerWateringSlot(cmdId, h, m) — trouve + supprime + sauvegarde
    ├── validateGardenerWateringSlot(slot)      — vérifie bornes des champs
    ├── hasGardenerTimeOverlap(slot)            — chevauchement temporel même vanne
    ├── countGardenerSlotsForValve(cmdId)       — nombre de créneaux pour une vanne
    ├── saveGardenerWateringSlots()             — écrit /gardener.json (atomique)
    ├── loadGardenerWateringSlots()             — lit /gardener.json au boot
    └── serializeGardenerWateringSlots()        — JSON commun persistance + MQTT ToUser
```

---

## Flux détaillés

### init() — au boot

1. `loadGardenerWateringSlots()` : ouvre `/gardener.json`, parse avec ArduinoJson, remplit `gardenerWateringSlots[]`. Si fichier absent ou JSON corrompu → liste vide + log warn/error.
2. `Console::info("Gardener", "Gardener démarré — N créneau(x) chargé(s)")`
3. Pas de publication MQTT ici (MQTT pas encore connecté).

### Publication au boot

Déclenchée par MqttManager dans `mqttEventHandler` sur `MQTT_EVENT_CONNECTED`, juste après `publishSchema()` : appel à `GardenerManager::publishGardenerWateringState()`. Même pattern que `publishOnline()` et `publishSchema()` existants. Cela permet à toute UI qui se connecte de recevoir immédiatement l'état courant du planning (grâce au retain).

### handle() — toutes les 1000 ms

1. Si `VClock_available == false` → return. Pendant les ~4 premières minutes après le boot, VirtualClock n'a pas encore basculé sur NTP ou RTC ; le scheduler reste inactif. Le flag `VClock_reliable` n'est pas consulté : la fiabilité de l'horloge est garantie par ailleurs et n'est pas du ressort du programmateur.
2. Traiter le buffer de messages MQTT entrants (si un message FromUser a été bufferisé par `onGardenerMessage()`).
3. Obtenir le timestamp UTC via `VirtualClock::read().timestamp`.
4. Convertir en heure locale via `localtime_r()`.
5. Calculer `currentMinute = hour * 60 + minute`.
6. Si `currentMinute == gardenerLastMinute` → return (même minute, déjà traitée).
7. Parcourir `gardenerWateringSlots[]` : pour chaque slot dont `slot.hour * 60 + slot.minute == currentMinute` :
   - Construire un BusItem et appeler `DataBus::publish()` (voir section « Déclenchement d'un arrosage »)
   - `Console::info("Gardener", "Arrosage auto vanne X — durée Y s")`
8. `gardenerLastMinute = currentMinute`.

### Pas de rattrapage

Si l'ESP était éteint ou rebootait au moment d'un créneau, ce créneau est définitivement manqué. Le scheduler n'agit qu'au moment où une nouvelle minute démarre et qu'un créneau y commence exactement.

### Mécanisme de détection de minute

Une seule variable `gardenerLastMinute` (init 0xFFFF) gère toutes les transitions :
- Passage de minuit (la minute repasse de 1439 à 0) : naturellement géré.
- Créneaux courts (1 s) : la minute ne change pas pendant 60 ticks, pas de re-déclenchement.
- Ticks multiples dans une même minute : déclenchement unique garanti.

---

## Déclenchement d'un arrosage — chaîne complète

### Rappel du format CSV 7 champs

En accord avec MetaDataModel toute donnée transitant par DataBus utilise un format unique à 7 champs :

`timestamp, VClock_available, VClock_reliable, type, id, valueType, value`

- **3 champs temps** : `timestamp`, `VClock_available`, `VClock_reliable` — fournis par DataBus via VirtualClock.
- **4 champs données** : `type`, `id`, `valueType`, `value` — fournis par le producteur dans un BusItem.

Pour les données courantes (capteurs, états), `type` est le DataType META de l'entité (Power, Sensor, Actuator, System). Pour les commandes, `type` est `CommandManual` (5) ou `CommandAuto` (6), qui qualifie l'origine de la commande.

### API DataBus — point d'entrée unique

DataBus expose une seule méthode publique de publication :

```cpp
static void publish(BusItem& item);
```

Le producteur construit un BusItem avec les 4 champs données, puis appelle `publish`. DataBus :
1. **Valide** le BusItem contre META (id, type, nature, bornes)
2. **Horodate** via `VirtualClock::read()` (les 3 champs temps)
3. **Distribue** vers mqttQueue, logQueue, WebServer
4. **Route** via `RELAYS[]` vers le manager si `type` == `CommandManual` ou `CommandAuto`

Si la validation échoue, le BusItem est rejeté (`Console::warn`, pas distribué).

Pour les entrées externes (MQTT `serre/cmd`, WebServer POST `/command`). parseCommand(csv) parse le CSV, valide, remplit un BusItem, puis l'appelant fait `DataBus::publish(item)`.

### Ce que GardenerManager fait

Pour chaque créneau qui se déclenche, GardenerManager construit un BusItem et le publie :

```cpp
BusItem item = {};
item.type       = DataType::CommandAuto;
item.id         = slot.cmdId;        // CommandValve1..6 (ids 17..22)
item.valueKind  = 0;
item.valueFloat = (float)slot.duration;  // secondes (META : 1.0–900.0)
DataBus::publish(item);
```

C'est le même pattern que tous les autres producteurs du système (FakeVoltage, WiFi, ValveManager, SmsManager...). La durée est en secondes, conformément à META (unité "s", bornes 1.0–900.0). La conversion en millisecondes est un détail interne de `DataBus::routeCommand()`.

### Chaîne existante après publish

`DataBus::publish` effectue automatiquement :
- Validation contre META (id valide, type cohérent, valeur dans les bornes)
- Horodatage via `VirtualClock::read()`
- Distribution vers mqttQueue → publication sur `serre/data/{id}` (l'UI voit la commande auto)
- Distribution vers logQueue → archivage LittleFS (DataLogger)
- Mise à jour WebServer (lastDataForWeb)
- Routage via `RELAYS[]` → `ValveManager::enqueueByEntity()` → ouverture physique de la vanne

L'anti-rebond de ValveManager (ignore une demande d'ouverture si la vanne est déjà ouverte) protège le cas où un arrosage manuel et un arrosage auto se chevauchent.

**Aucune modification nécessaire** dans DataBus, ValveManager, DataLogger, IO-Config.

---

## Protocole MQTT

### Topics

| Topic | Direction | Retain | Usage |
|---|---|---|---|
| `serre/gardener/FromUser` | UI → ESP | non | Demandes add/remove |
| `serre/gardener/ToUser` | ESP → UI | oui | État complet du planning |

### Payload add (FromUser) exemple

```json
{"op":"add","cmdId":17,"hour":7,"minute":0,"duration":300,"cancellableBySensor":false}
```

### Payload remove (FromUser) exemple

```json
{"op":"remove","cmdId":17,"hour":7,"minute":0}
```

### Payload ToUser (état complet, republié après chaque FromUser et à chaque connexion MQTT)

```json
{"slots":[
  {"cmdId":17,"hour":7,"minute":0,"duration":300,"cancellableBySensor":false},
  {"cmdId":18,"hour":18,"minute":30,"duration":600,"cancellableBySensor":false}
]}
```

### Champ "op"

Le champ `"op"` discrimine l'opération : `"add"` ou `"remove"`. Validé comme convention.

### Comportement après réception d'un FromUser

Toute réception de FromUser (acceptée ou rejetée par la validation ESP) déclenche une publication de ToUser reflétant l'état courant. Si la demande est acceptée, ce nouvel état contient la modification ; si elle est rejetée, l'état publié est inchangé. L'UI déduit l'échec d'une demande par l'absence du créneau attendu après timeout.

---

## onGardenerMessage() — réception MQTT

### Threading

`onGardenerMessage(data, len)` est appelée depuis le thread esp_mqtt (comme `dispatchCommand()` pour `serre/cmd`). Pour la sécurité LittleFS (écriture depuis le thread TaskManager uniquement), le message brut est **bufferisé** dans `onGardenerMessage()`, puis traité dans `handle()` au prochain tick.

### Flux dans handle() (après extraction du buffer)

1. Parser le JSON avec ArduinoJson (StaticJsonDocument).
2. Lire le champ `"op"`.
3. Si `"add"` : extraire tous les champs → construire un `GardenerWateringSlot` → `addGardenerWateringSlot()`.
4. Si `"remove"` : extraire `cmdId`, `hour`, `minute` → `removeGardenerWateringSlot()`.
5. **Dans tous les cas** (succès ou échec) : `publishGardenerWateringState()`.

---

## Validation à l'addition (addGardenerWateringSlot)

### Étape 1 — validateGardenerWateringSlot (bornes des champs)
Toujours se référer à MetaDataModel.h et IO-Config car cela peut évoluer 

Exemple :
- `cmdId` ∈ {17, 18, 19, 20, 21, 22} (les 6 CommandValve*).
- `duration` ∈ [1, 900] secondes (cohérent avec META min/max et `ValveManager::VALVE_MAX_DURATION_MS` = 15 min).
- `hour` ∈ [0, 23].
- `minute` ∈ [0, 59].
- `cancellableBySensor` : booléen accepté tel quel (en v1 ignoré au déclenchement).

### Étape 2 — contrôles de cohérence

- **Pas de doublon exact** : aucun créneau existant n'a le triplet (cmdId, hour, minute) identique.
- **Pas de chevauchement temporel** sur la même vanne : les minutes occupées par le créneau demandé ne doivent en intersecter aucune des minutes occupées par les créneaux existants pour cette même vanne. Un créneau occupe les minutes de `startMinute` à `startMinute + ceil(duration / 60) - 1`, modulo 1440 (gestion du passage de minuit).
- **Pas de saturation** : la vanne ciblée n'a pas déjà 6 créneaux.

### En cas de rejet  (proposition à améliorer)

Demande ignorée silencieusement côté logique métier (pas de modification de la liste, pas d'écriture LittleFS). Mais ToUser est republié quand même. Tout rejet est tracé via `Console::warn` avec la raison précise, **pour permettre le diagnostic à distance** : sans cette trace, un utilisateur qui voit son créneau « disparaître » de ToUser n'aurait aucun moyen de comprendre pourquoi.

### Suppression (removeGardenerWateringSlot)

1. Recherche linéaire du triplet (cmdId, hour, minute) dans le tableau.
2. Si trouvé : suppression par swap avec le dernier élément, décrémentation du compteur, sauvegarde, log info.
3. Si non trouvé : log warn, aucune modification.

---

## Validation côté UI — partage du fardeau

L'UI valide localement avant d'envoyer une demande, en s'appuyant sur la liste à jour reçue via `serre/gardener/ToUser` (retain). Si une demande est invalide selon ses règles, l'UI affiche « Validation impossible » et n'envoie rien.

La validation côté ESP32 couvre les cas rares :
- UI avec une vue périmée du planning (juste après reboot, message retain pas encore reçu).
- Deux UI concurrentes (téléphone + tablette) qui envoient un add quasi simultanément.
- Client non officiel ou UI buggée.
- JSON malformé.

C'est une **validation en profondeur** : l'UI filtre pour le confort utilisateur, l'ESP filtre pour l'intégrité de l'état. 

---

## Persistance LittleFS

### Stratégie : écriture immédiate à chaque modification

Chaque add/remove validé déclenche une sauvegarde dans `/gardener.json`. L'écriture est rare (quelques fois par jour maximum) et le fichier est petit (quelques centaines d'octets).

### Écriture atomique

1. `serializeGardenerWateringSlots()` → JSON string.
2. Écrire dans `/gardener.tmp` (open en mode write).
3. Fermer le fichier.
4. `LittleFS.rename("/gardener.tmp", "/gardener.json")` — atomique sur LittleFS.
5. Si crash pendant l'écriture du .tmp : au reboot, `/gardener.json` est intact.

### Lecture

Une seule fois au boot, dans `init()`. Si le fichier n'existe pas, démarrage avec liste vide. Si le JSON est malformé : log d'erreur, démarrage avec liste vide.  
Tous les arrosages programmmés depuis le boot sont bien sûr conservés en RAM, visibles dans les UI et utilisés.

### Format

Identique au payload ToUser :
```json
{"slots":[...]}
```
Une seule sérialisation/désérialisation dans tout le code.

### Bibliothèque JSON

ArduinoJson@^6.18.3, déjà disponible dans `platformio.ini`.

---

## Interférences des écritures DataLogger — analyse complète

### Résultat : aucune interférence

| Point vérifié | Résultat |
|---|---|
| Threading | Même thread TaskManager → exécution séquentielle, pas de concurrence |
| Fichiers | Fichiers différents (`log_*.csv` vs `gardener.*`) → pas de conflit de handles |
| Itération répertoire | DataLogger filtre sur `log_*.csv` → ignore `gardener.*` |
| Espace flash | `gardener.json` ≈ quelques centaines d'octets, négligeable sur partition 8 Mo |
| Rename atomique | Safe : GardenerManager ferme .tmp avant rename, DataLogger n'a aucun handle sur gardener.* |
| Latence | Écriture ~500 octets + rename = quelques ms, négligeable |

**Vérifié sur le code actuel** : `cleanupOldFiles()`, `clearHistory()` et `getFlashUsageStats()` dans `DataLogger.cpp` filtrent tous sur `log_*.csv` et ignorent tout autre fichier.

### Mutex LittleFS

Le wrapper `esp_littlefs` (intégré au core Arduino ESP32 v2+) protège toutes les opérations VFS avec un sémaphore récursif FreeRTOS. Cela constitue un filet de sécurité supplémentaire, même si l'exécution séquentielle dans TaskManager le rend superflu pour notre cas.

### ESP32-S3

- Aucun bug spécifique identifié pour LittleFS sur ESP32-S3.
- USB CDC désactivé dans le projet (`ARDUINO_USB_CDC_ON_BOOT=0` dans `platformio.ini`).
- Les contraintes SPI flash (désactivation du cache pendant les écritures) sont gérées automatiquement par ESP-IDF et n'impactent pas le fonctionnement.

---

## Intégration MQTT — modifications de MqttManager

### Principe

MqttManager reste un **passe-plat** : il ne connaît pas la sémantique du JSON Gardener. Il route le payload brut vers GardenerManager et expose une méthode de publication dédiée.

### Modifications dans MqttManager.h

- Ajout de la déclaration : `static void publishGardenerWateringState(const char* payload, size_t len);`

### Modifications dans MqttManager.cpp

1. **MQTT_EVENT_CONNECTED** : ajouter un `esp_mqtt_client_subscribe` sur `GARDENER_TOPIC_FROM_USER`, et un appel à `GardenerManager::publishGardenerWateringState()` pour la publication initiale (retain).

2. **MQTT_EVENT_DATA** : le handler actuel appelle `dispatchCommand()` qui vérifie le topic `serre/cmd`. Il faut élargir le routage :
   - Si topic = `serre/cmd` → traitement CSV existant (inchangé).
   - Si topic = `serre/gardener/FromUser` → appel à `GardenerManager::onGardenerMessage(data, len)`.

3. **Nouvelle méthode `publishGardenerWateringState(payload, len)`** : encapsule `esp_mqtt_client_publish()` avec retain=true sur `GARDENER_TOPIC_TO_USER`. Le topic et le flag retain sont connus en interne.

---

## Intégration dans main.cpp

### Include

```cpp
#include "Gardener/GardenerManager.h"
```

### Initialisation

`GardenerManager::init()` placée après `MqttManager::init()` et avant `TaskManager::init()`. GardenerManager a besoin que LittleFS soit initialisé (c'est le cas, fait dans `setup()`).

### Tâche périodique

```cpp
TaskManager::addTask(
    []() { GardenerManager::handle(); },
    GARDENER_HANDLE_PERIOD_MS
);
```

---

## Modification de TimingConfig.h

Conformément à la convention projet, le timing des tâches périodiques est défini dans `Config/TimingConfig.h`.

Ajout d'une constante :

```cpp
#define GARDENER_HANDLE_PERIOD_MS   1000
```

---

## Logs

### Console::info

- Au boot : `"Gardener démarré — N créneau(x) chargé(s)"`
- À chaque déclenchement : `"Arrosage auto vanne X — durée Y s"`
- À chaque add accepté : `"Créneau ajouté : vanne X à HH:MM pendant Y s"`
- À chaque remove accepté : `"Créneau supprimé : vanne X à HH:MM"`

### Console::warn

- JSON malformé
- Chevauchement détecté
- Doublon détecté
- Saturation (vanne déjà à 6 créneaux)
- cmdId invalide ou autre champ hors borne
- Remove d'un créneau inexistant

### Console::error

- Échec lecture/écriture LittleFS

---

## Interface utilisateur MQTT — RecepteurV6_serre.html

### Principe général

L'UI Gardener est intégrée dans le fichier HTML existant `RecepteurV6_serre.html` sous forme d'un **bloc séparé** placé après les cartes existantes (capteurs, vannes manuelles). Le bloc d'arrosage manuel et le bloc d'arrosage programmé sont deux fonctions indépendantes : aucune modification du code existant n'est nécessaire.

### Découplage du schéma META

Contrairement aux cartes capteurs/vannes qui sont générées dynamiquement depuis le schéma META reçu par MQTT, le bloc Gardener utilise une **structure codée en dur** dans le JS. Raisons :

- Le payload `serre/gardener/ToUser` porte déjà toute l'information nécessaire (slots avec cmdId, hour, minute, duration, cancellableBySensor).
- Les 6 vannes et leurs cmdId (17-22) sont stables (définis dans IO-Config, ne changent qu'à la recompilation).
- Le formulaire d'ajout a une structure propre (slider, sélecteurs heure/minute) qui n'a rien à voir avec les cartes META.

Une table de correspondance JS centralise le mapping :

```javascript
var GARDENER_VALVES = [
    { cmdId: 17, label: 'Vanne 1' },
    { cmdId: 18, label: 'Vanne 2' },
    { cmdId: 19, label: 'Vanne 3' },
    { cmdId: 20, label: 'Vanne 4' },
    { cmdId: 21, label: 'Vanne 5' },
    { cmdId: 22, label: 'Vanne 6' }
];
```

Quand les vannes seront renommées (ex : "Groupe Oliviers"), il suffira de modifier cette table.

### Constantes JS Gardener

```javascript
var GARDENER_TOPIC_FROM_USER = 'serre/gardener/FromUser';
var GARDENER_TOPIC_TO_USER   = 'serre/gardener/ToUser';
var GARDENER_MAX_SLOTS_PER_VALVE = 6;
var GARDENER_DURATION_MIN = 5;    // secondes
var GARDENER_DURATION_MAX = 900;  // secondes (15 min)
```

### Structure HTML du bloc Gardener

#### Titre de section

Un titre "Programmation arrosage" séparant visuellement le bloc Gardener des cartes existantes, avec le même style que les titres de groupes existants (classe `group-title`).

#### Une carte par vanne (les 6 toujours visibles)

Chaque carte contient deux zones :

**Zone haute — liste des créneaux programmés**

- Si aucun créneau : texte discret "Aucun créneau programmé".
- Si créneaux existants : liste compacte, un créneau par ligne contenant :
  - Heure au format HH:MM (font-weight 500).
  - Durée en format lisible (ex : "5 min", "2 min 30 s").
  - Bouton ✕ pour supprimer le créneau.
- Maximum 6 créneaux affichés (cohérent avec MAX_SLOTS_PER_VALVE).

**Zone basse — bouton d'ajout et formulaire repliable**

- Par défaut, seul un bouton "Ajouter un créneau" est visible.
- Au clic, le bouton disparaît et le formulaire se déplie (transition CSS) :
  - Sélecteur **heure** (0-23) et **minute** (0-59) via deux `<select>`.
  - **Slider logarithmique** pour la durée (voir section dédiée ci-dessous).
  - Bouton "Confirmer" (style succès) et bouton "Annuler".
- "Annuler" replie le formulaire et réaffiche le bouton "Ajouter".
- "Confirmer" déclenche la validation locale puis l'envoi MQTT.

**Champ cancellableBySensor** : non affiché en v1. Le champ est envoyé à `false` dans le payload JSON, mais aucun élément d'interface ne le montre. Il sera ajouté visuellement quand le module capteur sera disponible.

### Slider logarithmique de durée

#### Problème

Les durées d'arrosage vont de 5 secondes (brumisateur court) à 15 minutes (arrosage profond). Un slider linéaire sur cette plage est inutilisable : les courtes durées sont concentrées sur 3% du curseur.

#### Solution

Un `<input type="range">` à 100 positions (0-100) dont la valeur est convertie via une courbe exponentielle :

```
durée = 5 × 180^(position / 100)
```

Propriétés de cette courbe :
- Position 0 → 5 s
- Position 25 → ~15 s
- Position 50 → ~67 s (~1 min)
- Position 75 → ~310 s (~5 min)
- Position 100 → 900 s (15 min)

#### Arrondi adaptatif

La valeur brute est arrondie au pas le plus proche selon la zone :

| Plage          | Pas     | Exemples de valeurs                |
|---|---|---|
| < 30 s         | 5 s     | 5, 10, 15, 20, 25, 30             |
| 30 s – 120 s   | 10 s    | 30, 40, 50, 60, 70, 80, 90, …     |
| 120 s – 300 s  | 15 s    | 120, 135, 150, 165, 180, …        |
| 300 s – 900 s  | 30 s    | 300, 330, 360, 390, …, 870, 900   |

La valeur est clampée entre 5 et 900.

#### Affichage

À côté du slider, la durée est affichée en temps réel au format lisible :
- En dessous de 60 s : "25 s"
- Au-dessus de 60 s : "2 min 30 s", "5 min", "12 min 30 s"

Sous le slider, une échelle indicative fixe montre les repères : "5 s — 30 s — 2 min — 5 min — 15 min".

#### Implémentation JS

```javascript
function logSliderToSeconds(pos) {
    var raw = 5 * Math.pow(180, pos / 100);
    var step = raw < 30 ? 5 : raw < 120 ? 10 : raw < 300 ? 15 : 30;
    var snapped = Math.round(raw / step) * step;
    return Math.max(5, Math.min(900, snapped));
}

function formatDuration(sec) {
    sec = Math.round(sec);
    if (sec < 60) return sec + ' s';
    var m = Math.floor(sec / 60), s = sec % 60;
    return s === 0 ? m + ' min' : m + ' min ' + s + ' s';
}
```

### Abonnements MQTT

Au `client.on('connect')`, en plus des abonnements existants (TOPIC_SCHEMA, TOPIC_DATA, TOPIC_STATUS), ajouter :

```javascript
client.subscribe(GARDENER_TOPIC_TO_USER, { qos: 1 });
```

Dans `client.on('message')`, ajouter un bloc de routage **avant** le bloc `serre/data/` :

```javascript
if (topic === GARDENER_TOPIC_TO_USER) {
    handleGardenerState(payload);
    return;
}
```

### Réception ToUser — handleGardenerState(payload)

1. Parse le JSON : `{"slots":[...]}`.
2. Stocke la liste de slots dans une variable globale `gardenerSlots`.
3. Reconstruit l'affichage de toutes les cartes vannes Gardener :
   - Pour chaque vanne, filtre les slots dont le cmdId correspond.
   - Affiche la liste des créneaux ou "Aucun créneau programmé".
   - Replie le formulaire d'ajout s'il était ouvert (l'état vient de changer).
4. Pas de localStorage pour les slots Gardener : le retain MQTT garantit la réception de l'état courant à chaque connexion. Cela évite toute désynchronisation entre le cache local et l'état réel de l'ESP.

### Envoi FromUser — publication add/remove

#### Ajout (clic sur "Confirmer")

1. Lire les valeurs du formulaire (cmdId de la carte, heure, minute, durée via le slider).
2. Validation locale (voir section suivante).
3. Si valide, publier sur `serre/gardener/FromUser` :

```javascript
var payload = JSON.stringify({
    op: 'add',
    cmdId: cmdId,
    hour: hour,
    minute: minute,
    duration: logSliderToSeconds(sliderPosition),
    cancellableBySensor: false
});
client.publish(GARDENER_TOPIC_FROM_USER, payload, { qos: 1 });
```

4. Ne pas modifier l'affichage immédiatement : attendre le prochain ToUser (source de vérité).

#### Suppression (clic sur ✕)

1. Identifier le créneau (cmdId, hour, minute) depuis les data-attributes du bouton.
2. Pas de validation nécessaire pour un remove.
3. Publier :

```javascript
var payload = JSON.stringify({
    op: 'remove',
    cmdId: cmdId,
    hour: hour,
    minute: minute
});
client.publish(GARDENER_TOPIC_FROM_USER, payload, { qos: 1 });
```

4. Attendre le ToUser pour la mise à jour visuelle.

### Validation locale (avant envoi add)

L'UI valide en s'appuyant sur la liste `gardenerSlots` reçue via ToUser. Si la validation échoue, un message d'erreur s'affiche brièvement dans la carte concernée et aucun message MQTT n'est envoyé.

| Contrôle | Message utilisateur |
|---|---|
| Vanne déjà à 6 créneaux | "Maximum 6 créneaux atteint pour cette vanne" |
| Doublon exact (même cmdId + hour + minute) | "Un créneau existe déjà à cette heure" |
| Chevauchement temporel (même vanne) | "Ce créneau chevauche un créneau existant" |
| Durée hors bornes (< 5 s ou > 900 s) | Impossible via le slider (borné mécaniquement) |

#### Détection de chevauchement côté UI

Même algorithme que côté ESP : un créneau occupe les minutes de `startMinute` à `startMinute + ceil(duration / 60) - 1`, modulo 1440. Deux créneaux se chevauchent si leurs plages de minutes s'intersectent.

```javascript
function hasOverlap(cmdId, hour, minute, duration, slots) {
    var start = hour * 60 + minute;
    var span = Math.ceil(duration / 60);
    for (var i = 0; i < slots.length; i++) {
        var s = slots[i];
        if (s.cmdId !== cmdId) continue;
        if (s.hour === hour && s.minute === minute) continue; // doublon, pas chevauchement
        var sStart = s.hour * 60 + s.minute;
        var sSpan = Math.ceil(s.duration / 60);
        // Vérifier intersection modulo 1440
        for (var m = 0; m < span; m++) {
            var mine = (start + m) % 1440;
            for (var n = 0; n < sSpan; n++) {
                if (mine === (sStart + n) % 1440) return true;
            }
        }
    }
    return false;
}
```

Note : cette implémentation en O(n²) est parfaitement acceptable pour un maximum de 6 créneaux par vanne.

### Affichage des messages d'erreur

Un élément `<div>` dédié dans chaque carte vanne, masqué par défaut. En cas d'erreur de validation :
- Le message s'affiche avec un fond d'alerte léger (style existant du projet).
- Le message disparaît automatiquement après 4 secondes.
- Un seul message à la fois par carte.

### Style CSS

Le bloc Gardener reprend le vocabulaire visuel de l'UI existante (fond bleu, cartes semi-transparentes `rgba(255,255,255,0.2)`, coins arrondis 15px, police blanche). Les classes CSS Gardener sont préfixées `gd-` pour éviter toute collision avec les classes existantes.

Composants spécifiques :
- **Carte vanne Gardener** : même apparence que `.valve-card`, classe `gd-card`.
- **Ligne de créneau** : fond légèrement plus opaque `rgba(255,255,255,0.15)`, coins arrondis 8px.
- **Bouton supprimer (✕)** : discret par défaut, rouge au survol.
- **Formulaire repliable** : séparé par un trait horizontal fin, apparaît/disparaît via toggle de classe `open`.
- **Slider** : style natif du navigateur (acceptable sur mobile).
- **Échelle indicative** : petits labels sous le slider, opacité réduite.
- **Bouton "Confirmer"** : fond vert, texte blanc (même style que `.action-btn`).
- **Bouton "Annuler"** : style secondaire, transparent avec bordure.
- **Message d'erreur** : fond `rgba(244,67,54,0.3)`, coins arrondis, auto-dismiss 4 s.

---

### Fichiers NON modifiés

MetaDataModel.h, DataBus, ValveManager, DataLogger, IO-Config, Console, VirtualClock, NetworkConfig — aucune modification.

---

## Récapitulatif des décisions de design

| Décision | Choix |
|---|---|
| 6 créneaux par vanne, 36 au total | Confirmé |
| Identité (cmdId, hour, minute), pas de modification | Confirmé |
| Validation ESP32 comme maître final + validation UI pour confort | Confirmé |
| Pas de rattrapage en cas de boot tardif | Confirmé |
| Mécanisme unique gardenerLastMinute pour la détection de minute | Confirmé |
| Champ cancellableBySensor prévu mais ignoré en v1 | Confirmé |
| Champ "op" dans le JSON FromUser pour discriminer add/remove | Confirmé |
| Persistance immédiate à chaque modification | Confirmé |
| Messages MQTT Gardener hors DataBus (seul le déclenchement passe par DataBus) | Confirmé |
| MqttManager comme passe-plat (route le payload brut, ne parse pas le JSON) | Confirmé |
| Buffer du message FromUser dans onGardenerMessage(), traitement dans handle() | Confirmé |
| Toutes les écritures LittleFS depuis le thread TaskManager | Confirmé |
| Producteur construit le BusItem avec les 4 champs données, DataBus ajoute les 3 champs temps | Fait |
| MetaDataModel.h et IO-Config.h comme sources de vérité | Confirmé |
| UI Gardener : bloc séparé sous les cartes existantes, pas de modification du code existant | Confirmé |
| UI Gardener : structure codée en dur (pas construite depuis META) | Confirmé |
| UI Gardener : les 6 cartes vannes toujours visibles | Confirmé |
| UI Gardener : formulaire d'ajout repliable (caché par défaut, visible au clic "Ajouter") | Confirmé |
| UI Gardener : slider logarithmique 5 × 180^(pos/100), arrondi adaptatif | Confirmé |
| UI Gardener : cancellableBySensor non affiché en v1 (envoyé à false) | Confirmé |
| UI Gardener : pas de localStorage pour les slots (retain MQTT suffit) | Confirmé |
| UI Gardener : attendre ToUser pour mettre à jour l'affichage (pas de modification optimiste) | Confirmé |
