# Spécifications — Refactoring gestion du temps
## Projet Serre de Marie-Pierre — Waveshare ESP32-S3-Relay-6CH
## Version 2 — mise à jour après relecture

---

## 1. Architecture temps — Principes fondamentaux

### 1.1 Maître du temps : ManagerUTC
- ManagerUTC est le point d'accès unique au temps pour tout le système
- Il connaît les deux sources (RTCManager et VirtualClock)
- Il fait la cascade de dégradation en interne
- Il fournit un temps accompagné de deux booléens
- Il passe par RTCManager pour lire la carte RTC (jamais d'accès I2C direct)

### 1.2 Cascade de dégradation
1. **RTC OK** → UTC fiable, `available = true`, `reliable = true`
2. **RTC KO + VClock recalée** → UTC approximatif, `available = true`, `reliable = false`
3. **RTC KO + VClock jamais recalée** → millis, `available = false`, `reliable = false`

Dans tous les cas, `timestamp` contient une valeur exploitable :
- Cas 1 et 2 : un temps UTC
- Cas 3 : un temps en millisecondes depuis le boot

### 1.3 Deux axes indépendants
- **Axe métier** (la serre tourne) : VirtualClock toujours, même jamais recalée. L'arrosage et les capteurs fonctionnent indépendamment du temps fiable. La serre est totalement autonome.
- **Axe données** (on enregistre et on publie) : la cascade à trois niveaux avec fallback millis dans Pending.

### 1.4 Le millis est toujours capturé
- `readUTC()` retourne `millis()` dans `timestamp` quand `available = false`
- C'est le filet de sécurité ultime si aucun temps UTC n'est disponible
- DataLogger n'a plus besoin de capturer millis séparément

### 1.5 Séparation affichage / stockage
- **Ce qu'on affiche** (local et téléphone) peut inclure du temps millis ("Depuis 25s")
- **Ce qu'on stocke** (SPIFFS et serveur distant) ne contient que des records avec `available == true`
- Ce sont deux chemins séparés, pas de doublon

---

## 2. Nouvelle struct TimeUTC

Définie dans ManagerUTC.h :

```cpp
struct TimeUTC {
    time_t  timestamp;  // UTC si available, millis si !available
    bool    available;  // true = un temps UTC existe
    bool    reliable;   // true = RTC, false = VClock
};
```

Méthode d'accès :

```cpp
static TimeUTC readUTC();
```

Usage type :

```cpp
TimeUTC t = ManagerUTC::readUTC();
if (t.available && t.reliable)  { /* RTC */ }
if (t.available && !t.reliable) { /* VClock */ }
if (!t.available)               { /* millis dans t.timestamp */ }
```

---

## 3. Deux flux MQTT distincts

### 3.1 Flux live : `serre/live`
- Publié à chaque `push()`, même contenu que `LastDataForWeb`
- Le téléphone s'abonne et affiche exactement la même chose que la page Web locale
- Peut contenir du temps millis (affichage "Depuis Xs") quand `available == false`
- **Règle : affichage local = affichage téléphone**

### 3.2 Flux data : `serre/data`
- Publié au moment du flush SPIFFS, même contenu que le fichier CSV
- Le serveur distant s'abonne et reçoit exactement ce qui est stocké localement
- Ne contient que des records avec `available == true`
- Format : `timestamp,available,reliable,type,id,valueType,value`
- **Règle : SPIFFS = serveur distant**

---

## 4. Rôles des modules

### 4.1 RTCManager (Core/) — Pilote matériel DS3231
- Lecture/écriture I2C du DS3231 via RTClib
- Détection de panne par ping I2C (`Wire.beginTransmission(0x68)` + `Wire.endTransmission()`) dans `read()`, AVANT l'appel à `rtc.now()`
- Si le ping échoue → `_reliable = false`, retourne 0
- Vérification OSF uniquement au boot dans `init()` (déjà en place)
- RTClib reste en place (testé, validé, fiable sur ESP32-S3)
- Le ping I2C est rapide (~90µs à 100kHz) et non bloquant

### 4.2 VirtualClock (Core/) — Horloge de backup
- Toujours active pour le métier (arrosage, capteurs)
- `isSynced()` indique si elle a été recalée au moins une fois
- Recalée par RTC et par NTP via `sync()`
- Dérive sur millis() entre deux recalages
- `nowVirtual()` interpole : `_anchorUtc + (millis() - _anchorMillis) / 1000`
- `handle()` appelé toutes les 60s mais ne resync depuis RTC que toutes les 24h

### 4.3 ManagerUTC (Connectivity/) — Maître du temps

#### Rôle existant inchangé : agent NTP
- `init()` et `handle()` gèrent la synchro NTP
- Au boot : tentatives rapides (30s) jusqu'à 10 essais après 60s de réseau stable
- En régime : resync toutes les 24h
- À chaque synchro NTP réussie → écrit dans RTCManager ET sync VirtualClock

#### Nouveau rôle : fournisseur de temps
- `readUTC()` fait la cascade RTC → VClock → millis
- Struct `TimeUTC` définie ici

#### Nouveau rôle : mise à jour active de VirtualClock

**Problème constaté :** Si le RTC est en panne au boot et que NTP échoue 10 fois, ManagerUTC passe en rythme 24h. Pendant ce temps, VirtualClock reste sur midi arbitraire. Personne ne cherche à la mettre à jour. La serre tourne pendant 24h avec une heure complètement fausse. Les actions métier (arrosage) se déclenchent aux mauvais moments.

**Causes :**
- VirtualClock::handle() ne resync depuis le RTC que toutes les 24h
- ManagerUTC abandonne les tentatives rapides après 10 échecs NTP
- Si le RTC revient entre-temps (pile remplacée, reconnexion I2C), personne ne le détecte avant le prochain cycle de 24h

**Solution :** ManagerUTC doit prendre en charge la mise à jour de VClock activement tant qu'elle n'est pas synced :
- Vérification régulière du RTC (ex: toutes les 30s). Si `RTCManager::isReliable()` → `VirtualClock::sync()` immédiatement
- Poursuite des tentatives NTP, sans délai de 24h
- Dès qu'une source est disponible, sync immédiate
- Une fois VClock synced, passage au rythme normal de 24h

Détails d'implémentation à définir au moment du codage.

### 4.4 DataLogger (Storage/) — Enregistrement des données
- Appelle `ManagerUTC::readUTC()` dans `push()`
- Un seul chemin : `t.timestamp` contient toujours un temps, `t.available` et `t.reliable` disent quoi en faire
- Le Live utilise toujours VirtualClock
- Gère le fallback millis dans Pending quand `available == false`

### 4.5 PagePrincipale (Web/) — Affichage
- Affiche l'heure si `available == true` et `reliable == true`
- Affiche l'heure + "Non fiable" si `available == true` et `reliable == false`
- Affiche "Depuis Xs" via `t_millis` si `available == false`

---

## 5. Modifications des structures de données

### 5.1 Suppression de l'enum TimeBase
L'enum `TimeBase` (Relative/UTC) est supprimé. Remplacé par le booléen `available` partout.

### 5.2 Struct DataRecord (DataLogger.h)

Avant :
```cpp
struct DataRecord {
    uint32_t timestamp;
    TimeBase timeBase;
    DataType type;
    DataId   id;
    std::variant<float, String> value;
};
```

Après :
```cpp
struct DataRecord {
    uint32_t timestamp;
    bool     available;    // true = UTC, false = millis
    bool     reliable;     // true = RTC, false = VClock ou réparé
    DataType type;
    DataId   id;
    std::variant<float, String> value;
};
```

### 5.3 Struct LastDataForWeb (DataLogger.h)

Avant :
```cpp
struct LastDataForWeb {
    std::variant<float, String> value;
    uint32_t  t_rel_ms  = 0;
    time_t    t_utc     = 0;
    bool      utc_valid = false;
};
```

Après :
```cpp
struct LastDataForWeb {
    std::variant<float, String> value;
    time_t    t_utc     = 0;
    bool      available = false;
    bool      reliable  = false;
    uint32_t  t_millis  = 0;      // toujours présent, pour l'affichage "Depuis Xs"
};
```

Valeurs par défaut (0, false, false, 0) : état initial pour un DataId qui n'a jamais reçu de donnée. Écrasé dès le premier `push()`, ou à l'`init()` lors de la relecture SPIFFS.

### 5.4 Format CSV (identique local SPIFFS et flux MQTT data)

Avant : `timestamp,type,id,valueType,value` (5 champs)

Après : `timestamp,available,reliable,type,id,valueType,value` (7 champs)

---

## 6. Modifications par fichier

### 6.1 ManagerUTC.h
- Ajout de la struct `TimeUTC` (avec champ `timestamp`, pas `utc`)
- Ajout de la méthode `static TimeUTC readUTC()`

### 6.2 ManagerUTC.cpp
- Implémentation de `readUTC()` avec la cascade RTC → VClock → millis
- Quand `available = false`, `timestamp` contient `millis()`
- Ajout dans `handle()` : tant que VClock n'est pas synced, vérification régulière du RTC pour sync immédiate (détails d'implémentation au codage)

### 6.3 RTCManager.cpp
- `read()` : ajout du ping I2C avant `rtc.now()`. Si échec → `_reliable = false`, retourne 0

### 6.4 DataLogger.h
- Suppression de l'enum `TimeBase`
- Modification de `DataRecord` : remplacement de `TimeBase timeBase` par `bool available` + `bool reliable`
- Modification de `LastDataForWeb` : suppression `t_rel_ms`, renommage `utc_valid` → `available` + `reliable`, ajout `t_millis`
- Suppression de `getCurrentValueWithTime()` (déclarée, jamais implémentée, jamais appelée)
- Suppression de `getLast()` (déclarée, jamais implémentée, jamais appelée)

### 6.5 DataLogger.cpp

#### push() (float et String)
- Appeler `ManagerUTC::readUTC()` — `timestamp` contient toujours un temps
- Un seul chemin pour PENDING (plus de if/else millis vs UTC) :
  - `pendRec.timestamp = static_cast<uint32_t>(t.timestamp)`
  - `pendRec.available = t.available`
  - `pendRec.reliable = t.reliable`
- LIVE : inchangé (VirtualClock)
- LastDataForWeb : toujours alimenté
  - Si `t.available` : stocker `t_utc`, `available`, `reliable`
  - Toujours stocker `t_millis` (pour l'affichage "Depuis Xs" quand available = false)
- Flux MQTT live : publier le même contenu que LastDataForWeb (implémentation future)

#### handle()
- Réparation : utiliser `ManagerUTC::readUTC()`
- Si `t.available` → convertir les records millis (`available == false`) via `RTCManager::convertFromRelative()`
- Les records réparés reçoivent `available = true` et `reliable = t.reliable`
  - Cela signifie que `reliable` reflète l'état de la source au MOMENT DE LA RÉPARATION :
    - Si la réparation est faite quand le RTC est fiable → `reliable = true` (conversion juste)
    - Si la réparation est faite quand seule VClock est disponible → `reliable = false` (conversion approximative)

#### tryFlush()
- Suppression de la garde `RTCManager::isReliable()` en entrée
- On flushe tout record avec `available == true`, fiable ou pas
- La contrainte FIFO ne concerne que `addPending()` en cas de débordement (écriture)
- Le flush n'a pas de contrainte FIFO sur la lecture
- Flux MQTT data : publier le même contenu que ce qui est flushé vers SPIFFS (implémentation future)

#### flushToFlash()
- Nouveau format CSV à 7 champs : `timestamp,available,reliable,type,id,valueType,value`

#### init()
- Parser le nouveau format à 7 champs (2 champs de plus à lire par ligne)
- Lire `available` et `reliable` depuis le fichier CSV sans interprétation
- Ne pas hardcoder de valeurs : on lit et on écrit la réalité du SPIFFS

#### getLastUtcRecord()
- Parser le nouveau format à 7 champs

#### getGraphCsv()
- Parser le nouveau format à 7 champs

### 6.6 PagePrincipale.cpp

#### timeHtml()
- Si `available == true` et `reliable == true` : afficher l'heure formatée
- Si `available == true` et `reliable == false` : afficher l'heure formatée + indication "Non fiable"
- Si `available == false` : afficher "Depuis Xs" calculé à partir de `t_millis` (comportement existant conservé)

### 6.7 WebServer.cpp

#### buildBundleHeader()
- Mettre à jour le schéma JSON pour décrire le nouveau format CSV à 7 champs
- Documenter les champs `available` et `reliable` dans le schéma

### 6.8 TimingConfig.h
- Corriger le commentaire qui référence encore "ManagerUTC::handle()" (cosmétique)

---

## 7. Points NON modifiés

- VirtualClock : aucun changement de code (sauf éventuellement handle() — à détailler au codage)
- RTCManager : `init()`, `write()`, `convertFromRelative()` inchangés
- PageLogs : aucune dépendance au temps, pas d'impact
- WebServer.cpp : routes et handlers inchangés (sauf `buildBundleHeader()`)
- NetworkConfig.h, Config.h : aucun impact

---

## 8. Actions reportées (hors périmètre)

- Renommage ManagerUTC → NTPManager ou TimeManager (passe ultérieure)
- PageCapteurs, PageArrosage, PageReglages (n'existent pas encore)
- Implémentation MQTT (ultérieure, mais le format et les flux sont définis)

---

## 9. Prérequis déploiement

- **Suppression obligatoire de l'historique** avant déploiement (ancien format 5 champs incompatible avec nouveau format 7 champs)
- La page Logs permet déjà cette opération

---

## 10. Principe de nommage — cohérence partout

Les mêmes deux booléens (`available`, `reliable`) sont utilisés dans :
- `TimeUTC` (retour de `ManagerUTC::readUTC()`)
- `DataRecord` (stockage interne)
- `LastDataForWeb` (affichage Web)
- Format CSV (fichier SPIFFS)
- Flux MQTT data (serveur distant, futur)
- Flux MQTT live (téléphone, futur)

Même nom, même signification, pas de traduction entre les modules.

---

## 11. Points ouverts — en réflexion

- **Champ `timestamp` dans `TimeUTC`** : contient UTC ou millis selon `available`. Le nom `timestamp` est validé mais reste ouvert à reconsidération si nécessaire.
