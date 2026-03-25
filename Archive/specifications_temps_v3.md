# Spécifications — Refactoring gestion du temps
## Projet Serre de Marie-Pierre — Waveshare ESP32-S3-Relay-6CH
## Version 3 — mise à jour après implémentation et validation

---

## 1. Architecture temps — Principes fondamentaux

### 1.1 Maître du temps : ManagerUTC
- ManagerUTC est le point d'accès unique au temps pour tout le système
- Il connaît les deux sources (RTCManager et VirtualClock)
- Il fait la cascade de dégradation en interne
- Il fournit un temps accompagné de deux booléens
- Il passe par RTCManager pour lire la carte RTC (jamais d'accès I2C direct)

### 1.2 Cascade de dégradation
1. **RTC OK** → UTC fiable, `UTC_available = true`, `UTC_reliable = true`
2. **RTC KO + VClock recalée** → UTC approximatif, `UTC_available = true`, `UTC_reliable = false`
3. **RTC KO + VClock jamais recalée** → millis, `UTC_available = false`, `UTC_reliable = false`

Dans tous les cas, `timestamp` contient une valeur exploitable :
- Cas 1 et 2 : un temps UTC
- Cas 3 : un temps en millisecondes depuis le boot

### 1.3 Deux axes indépendants
- **Axe métier** (la serre tourne) : VirtualClock toujours, même jamais recalée. L'arrosage et les capteurs fonctionnent indépendamment du temps fiable. La serre est totalement autonome.
- **Axe données** (on enregistre et on publie) : la cascade à trois niveaux avec fallback millis dans Pending.

### 1.4 Le millis est toujours capturé
- `readUTC()` retourne `millis()` dans `timestamp` quand `UTC_available = false`
- C'est le filet de sécurité ultime si aucun temps UTC n'est disponible
- DataLogger n'a plus besoin de capturer millis séparément

### 1.5 Séparation affichage / stockage
- **Ce qu'on affiche** (local et téléphone) peut inclure du temps millis ("Depuis 25s")
- **Ce qu'on stocke** (SPIFFS et serveur distant) ne contient que des records avec `UTC_available == true`
- Ce sont deux chemins séparés, pas de doublon

### 1.6 Timezone — point unique de configuration
- La timezone système est configurée **une seule fois** dans `main.cpp` au boot :
  `setenv("TZ", SYSTEM_TIMEZONE, 1); tzset();`
- La constante `SYSTEM_TIMEZONE` est définie dans `Config/Config.h`
- Aucun autre module ne fait `setenv` ni `tzset`
- `configTzTime()` dans ManagerUTC est idempotent (API ESP-IDF pour SNTP, reconfigure la même TZ)
- `localtime_r()` fonctionne correctement partout, été comme hiver, grâce à la chaîne POSIX
  `"CET-1CEST,M3.5.0/2,M10.5.0/3"` qui encode les deux fuseaux et les dates de basculement
- Le code interne travaille **exclusivement en UTC ou millis** ; la conversion locale n'est faite qu'à l'affichage

### 1.7 time_t est 32 bits
- Platform `espressif32@6.3.0` (ESP-IDF 4.4) → `sizeof(time_t) = 4`
- Les casts `static_cast<uint32_t>(t.timestamp)` sont corrects
- `uint32_t` overflow en 2106, sans conséquence pour ce projet

---

## 2. Struct TimeUTC

Définie dans ManagerUTC.h :

```cpp
struct TimeUTC {
    time_t  timestamp;      // UTC si UTC_available, millis() sinon
    bool    UTC_available;   // true = timestamp est un temps UTC
    bool    UTC_reliable;    // true = RTC (précis), false = VClock (dérive)
};
```

Méthode d'accès :

```cpp
static TimeUTC readUTC();
```

Usage type :

```cpp
TimeUTC t = ManagerUTC::readUTC();
if (t.UTC_available && t.UTC_reliable)   { /* RTC */ }
if (t.UTC_available && !t.UTC_reliable)  { /* VClock */ }
if (!t.UTC_available)                    { /* millis dans t.timestamp */ }
```

---

## 3. Deux flux MQTT distincts

### 3.1 Flux live : `serre/live`
- Publié à chaque `push()`, même contenu que `LastDataForWeb`
- Le téléphone s'abonne et affiche exactement la même chose que la page Web locale
- Peut contenir du temps millis (affichage "Depuis Xs") quand `UTC_available == false`
- **Règle : affichage local = affichage téléphone**

### 3.2 Flux data : `serre/data`
- Publié au moment du flush SPIFFS, même contenu que le fichier CSV
- Le serveur distant s'abonne et reçoit exactement ce qui est stocké localement
- Ne contient que des records avec `UTC_available == true`
- Format : `timestamp,UTC_available,UTC_reliable,type,id,valueType,value`
- **Règle : SPIFFS = serveur distant**

---

## 4. Rôles des modules

### 4.1 RTCManager (Core/) — Pilote matériel DS3231

Aucune variable d'état persistante. Chaque opération est autonome.

#### init()
- `Wire.begin()` + `rtc.begin()` : initialisation I2C et détection du chip
- Vérification OSF via `rtc.lostPower()` : log d'avertissement si pile HS
- Ne positionne aucune variable ; le diagnostic est refait à chaque `read()`

#### read(time_t& rtcOut) — enchaînement de tests
1. `rtc.lostPower()` → true → return false (OSF actif ou bus I2C en panne)
   - Si le bus est en panne, `lostPower()` retourne true (mode de défaillance sûr)
2. `rtc.now()` → lecture du temps
3. Validation : timestamp < 1700000000 → return false (valeur aberrante)
4. Succès → return true

Chaque échec est propre, le prochain appel réessaie tout depuis le début.
Pas de ping I2C séparé : `lostPower()` fait un accès I2C qui sert de vérification.

#### write(time_t utc)
- Ping I2C (`Wire.beginTransmission(0x68)`) avant écriture
- `rtc.adjust()` écrit le temps ET efface OSF automatiquement
- Vérification OSF après écriture : si toujours actif → problème matériel
- Après un succès NTP → `write()` → OSF clair → le prochain `read()` réussit

#### Récupération automatique
- Pile morte au boot → OSF actif → `read()` retourne false → données en millis
- NTP réussit → `write()` → `adjust()` efface OSF → `read()` fonctionne de nouveau
- Hoquet I2C transitoire → `read()` retourne false → réessai automatique au prochain appel

### 4.2 VirtualClock (Core/) — Horloge de backup
- Composant passif : pas de `handle()`, recalée uniquement par ManagerUTC
- Toujours active pour le métier (arrosage, capteurs)
- `isVClockSynced()` indique si elle a été recalée au moins une fois
- Recalée par RTC au boot et par NTP via `sync()`
- Dérive sur millis() entre deux recalages
- `nowVirtual()` interpole : `_anchorUtc + (millis() - _anchorMillis) / 1000`
- Initialisée à 12h30 heure locale (ancre arbitraire CET)

### 4.3 ManagerUTC (Connectivity/) — Maître du temps

#### Rôle 1 : fournisseur de temps
- `readUTC()` fait la cascade RTC → VClock → millis
- Struct `TimeUTC` définie ici
- `readUTC()` appelle `RTCManager::read()` directement (pas de garde préalable)

#### Rôle 2 : agent NTP
- `init()` et `handle()` gèrent la synchro NTP
- Au boot : tentatives rapides (30s) jusqu'à 10 essais après 60s de réseau stable
- En régime : fréquence basée sur `VirtualClock::isVClockSynced()` :
  - VClock pas synced → un essai toutes les 50 min (récupération rapide)
  - VClock synced → un essai toutes les 25 × 50 min ≈ 20h50 (maintenance)
- À chaque synchro NTP réussie → écriture RTCManager + sync VirtualClock

#### Scénario dégradé (RTC KO + NTP KO)
- VClock reste à 12h30 arbitraire
- Les données sont horodatées en millis
- NTP est tenté toutes les 50 min
- Dès que NTP réussit : write() dans RTC, sync VClock, réparation des records millis

### 4.4 DataLogger (Storage/) — Enregistrement des données
- Appelle `ManagerUTC::readUTC()` dans `push()`
- Un seul chemin : `t.timestamp` contient toujours un temps, `t.UTC_available` et `t.UTC_reliable` disent quoi en faire
- Le Live utilise toujours VirtualClock
- Gère le fallback millis dans Pending quand `UTC_available == false`

### 4.5 PagePrincipale (Web/) — Affichage
- Affiche l'heure si `UTC_available == true` et `UTC_reliable == true`
- Affiche l'heure + "(Imprécis)" si `UTC_available == true` et `UTC_reliable == false`
- Affiche "Depuis Xs" si `UTC_available == false` (calculé à partir de `millis() - timestamp`)
- Utilise `localtime_r()` pour la conversion UTC → heure locale (TZ configurée au boot)

---

## 5. Structures de données

### 5.1 Suppression de l'enum TimeBase
L'enum `TimeBase` (Relative/UTC) est supprimé. Remplacé par le booléen `UTC_available` partout.

### 5.2 Struct DataRecord (DataLogger.h)

```cpp
struct DataRecord {
    uint32_t timestamp;      // UTC si UTC_available, millis() sinon
    bool     UTC_available;  // true = timestamp est un temps UTC
    bool     UTC_reliable;   // true = RTC, false = VClock ou réparé
    DataType type;
    DataId   id;
    std::variant<float, String> value;
};
```

### 5.3 Struct LastDataForWeb (DataLogger.h)

```cpp
struct LastDataForWeb {
    std::variant<float, String> value;
    time_t    timestamp     = 0;      // UTC ou millis selon UTC_available
    bool      UTC_available = false;
    bool      UTC_reliable  = false;
};
```

Valeurs par défaut (0, false, false) : état initial pour un DataId qui n'a jamais reçu de donnée. Écrasé dès le premier `push()`, ou à l'`init()` lors de la relecture SPIFFS.

Quand `UTC_available == false`, `timestamp` contient `millis()`. PagePrincipale calcule l'ancienneté via `millis() - timestamp`. Pas besoin d'un champ `t_millis` dédié.

### 5.4 Format CSV (identique local SPIFFS et flux MQTT data)

`timestamp,UTC_available,UTC_reliable,type,id,valueType,value` (7 champs)

---

## 6. Détails par fichier

### 6.1 ManagerUTC.h
- Struct `TimeUTC` avec champs `timestamp`, `UTC_available`, `UTC_reliable`
- Méthode `static TimeUTC readUTC()`

### 6.2 ManagerUTC.cpp
- `readUTC()` : cascade `RTCManager::read()` → `VirtualClock` → `millis()`
- Quand `UTC_available = false`, `timestamp` contient `millis()`
- `handle()` : agent NTP avec fréquence adaptative (50 min ou 25 × 50 min)

### 6.3 RTCManager.h
- API publique : `init()`, `read(time_t&)`, `write(time_t)`
- Aucune variable d'état privée, aucun getter d'état

### 6.4 RTCManager.cpp
- `init()` : Wire.begin + rtc.begin + log OSF (pas de variables)
- `read()` : lostPower → rtc.now → validation timestamp (pas de ping séparé)
- `write()` : ping I2C → rtc.adjust → vérification OSF

### 6.5 DataLogger.h
- Suppression de l'enum `TimeBase`
- `DataRecord` avec `UTC_available` + `UTC_reliable`
- `LastDataForWeb` avec `timestamp`, `UTC_available`, `UTC_reliable`
- Suppression de `getCurrentValueWithTime()` et `getLast()` (jamais implémentées)

### 6.6 DataLogger.cpp

#### push() (float et String)
- `ManagerUTC::readUTC()` fournit toujours un temps
- PENDING : `pendRec.timestamp = static_cast<uint32_t>(t.timestamp)`, `pendRec.UTC_available`, `pendRec.UTC_reliable`
- LIVE : VirtualClock (axe métier)
- LastDataForWeb : toujours alimenté avec timestamp, UTC_available, UTC_reliable
- Flux MQTT live : même contenu que LastDataForWeb (implémentation future)

#### handle()
- Réparation via `ManagerUTC::readUTC()`
- Calcul direct : `UTC_event = UTC_now - (millis_now - millis_event) / 1000`
- Les records réparés reçoivent `UTC_available = true` et `UTC_reliable = t.UTC_reliable`
  (reflète l'état de la source au moment de la réparation)

#### tryFlush()
- Flushe les records `UTC_available == true` contigus depuis la tête
- La contrainte FIFO en lecture est héritée de la FIFO en écriture
- En pratique, la réparation dans handle() convertit tous les records millis avant le flush,
  donc le break sur non-available ne se déclenche pas quand UTC est disponible
- Flux MQTT data : même contenu que SPIFFS (implémentation future)

#### flushToFlash()
- Format CSV à 7 champs : `timestamp,UTC_available,UTC_reliable,type,id,valueType,value`

#### init()
- Parse le format à 7 champs
- Lit `UTC_available` et `UTC_reliable` depuis le CSV sans interprétation

#### getLastUtcRecord(), getGraphCsv()
- Parsent le format à 7 champs

### 6.7 PagePrincipale.cpp

#### formatUtc()
- Utilise `localtime_r()` (thread-safe, TZ configurée au boot)

#### timeHtml()
- `UTC_available && UTC_reliable` : heure formatée
- `UTC_available && !UTC_reliable` : heure formatée + "(Imprécis)"
- `!UTC_available` : "Depuis Xs" calculé à partir de `millis() - timestamp`

### 6.8 WebServer.cpp

#### buildBundleHeader()
- Schéma JSON avec `csvColumns` : `["timestamp", "UTC_available", "UTC_reliable", "type", "id", "valueType", "value"]`

### 6.9 main.cpp

#### Séquence de boot
1. `setenv("TZ", SYSTEM_TIMEZONE, 1); tzset();` — timezone unique pour tout le firmware
2. `RTCManager::init()` — détection DS3231 + log OSF
3. `VirtualClock::init()` — ancre à 12h30
4. `RTCManager::read()` → si succès → `VirtualClock::sync()` — sync VClock sur RTC
5. `ManagerUTC::init()` — agent NTP

---

## 7. Points NON modifiés

- VirtualClock : aucun changement de code, composant passif
- PageLogs : aucune dépendance au temps, pas d'impact
- WebServer.cpp : routes et handlers inchangés (sauf `buildBundleHeader()`)
- NetworkConfig.h : aucun impact

---

## 8. Actions reportées (hors périmètre)

- Renommage ManagerUTC → NTPManager ou TimeManager (passe ultérieure)
- PageCapteurs, PageArrosage, PageReglages (n'existent pas encore)
- Implémentation MQTT (ultérieure, mais le format et les flux sont définis)

---

## 9. Prérequis déploiement

- **Suppression obligatoire de l'historique** avant déploiement (ancien format 5 champs incompatible avec nouveau format 7 champs)
- La page Logs permet cette opération
- **Mise à jour du script Python** `SerreBundleToXlsx.py` pour parser le format 7 champs

---

## 10. Principe de nommage — cohérence partout

Les mêmes deux booléens (`UTC_available`, `UTC_reliable`) sont utilisés dans :
- `TimeUTC` (retour de `ManagerUTC::readUTC()`)
- `DataRecord` (stockage interne)
- `LastDataForWeb` (affichage Web)
- Format CSV (fichier SPIFFS)
- Schéma JSON du bundle (WebServer)
- Flux MQTT data (serveur distant, futur)
- Flux MQTT live (téléphone, futur)
- Script Python `SerreBundleToXlsx.py` (conversion bundle → xlsx)

Même nom, même signification, pas de traduction entre les modules.

Dans le fichier xlsx généré par le script Python :
- `UTC_available` est affiché sous l'entête **"Temps"** avec les valeurs "UTC" ou "Millis"
- `UTC_reliable` est affiché sous l'entête **"Source"** avec les valeurs "RTC", "VClock", ou "—" (si millis)
