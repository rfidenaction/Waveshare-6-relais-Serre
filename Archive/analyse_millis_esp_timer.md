# Analyse — Remplacement millis() par esp_timer_get_time()
## Projet Serre de Marie-Pierre — Waveshare ESP32-S3-Relay-6CH
## Correction critique — Débordement millis() après 49.7 jours

---

## 1. Le problème

`millis()` retourne un `uint32_t` (32 bits non signé). Sa valeur maximale est 4 294 967 295, atteinte après environ 49.7 jours. Ensuite il repart à zéro.

La serre peut rester isolée (sans NTP ni RTC fonctionnel) pendant plus de 50 jours. Dans ce cas, les timestamps basés sur millis() deviennent faux après le débordement.

### 1.1 Où millis() est utilisé dans le code actuel

**VirtualClock (Core/)**
- `_anchorMillis` : point d'ancrage pour le calcul de `nowVirtual()`
- `init()` et `sync()` : `_anchorMillis = millis()`
- `nowVirtual()` : `millis() - _anchorMillis` → le delta peut dépasser 49.7 jours si aucun sync n'est fait

**ManagerUTC (Connectivity/)**
- `readUTC()` cas 3 : retourne `millis()` dans `timestamp` quand aucun UTC n'est disponible
- `handle()` : `_lastTourMs`, `_lastAttemptMs`, `_networkUpSinceMs` — intervalles courts, pas de problème

**DataLogger (Storage/)**
- `push()` : le timestamp millis du cas 3 de readUTC() est stocké dans PENDING comme `uint32_t`
- `handle()` réparation : `millis() - record.timestamp` pour calculer le delta → faux si millis a débordé entre le push et la réparation

**PagePrincipale (Web/)**
- `timeHtml()` : `millis() - d.timestamp` pour afficher "Depuis Xs" → même problème

### 1.2 Scénario concret de bug

1. Jour 0 : RTC en panne, pas de NTP. readUTC() retourne millis.
2. Jour 1 : DataLogger::push() stocke timestamp = 86 400 000 (millis à J+1)
3. Jour 50 : millis() a débordé et vaut ~300 000 (petit nombre)
4. DataLogger::handle() tente la réparation : `millis() - record.timestamp` = 300 000 - 86 400 000 → résultat faux en arithmétique uint32_t
5. NTP arrive enfin. Le timestamp réparé est aberrant.

### 1.3 Pourquoi la solution "ré-ancrage périodique" ne suffit pas

Même si VirtualClock se ré-ancre toutes les 24h sur elle-même, le problème persiste dans DataLogger : les records en PENDING portent un timestamp millis qui peut être d'il y a 49+ jours. La soustraction `millis() - record.timestamp` sera fausse.

Le problème est systémique, pas limité à VirtualClock.

---

## 2. La solution — esp_timer_get_time()

### 2.1 Disponibilité sur ESP32-S3

L'ESP32-S3 dispose du périphérique SYSTIMER : un compteur 64 bits toujours actif, cadencé par XTAL_CLK / 2. L'API `esp_timer_get_time()` retourne ce compteur en microsecondes sous forme de `int64_t`.

Capacité : un `int64_t` en microsecondes ne déborde pas avant ~292 000 ans. Le problème disparaît.

Documentation Espressif officielle : https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/esp_timer.html

### 2.2 Principe

Remplacer `millis()` par `esp_timer_get_time() / 1000` (microsecondes → millisecondes) partout où un timestamp relatif au boot est stocké ou comparé sur de longues durées.

Ou mieux : stocker directement en microsecondes pour garder la précision maximale dans VirtualClock, et convertir en millisecondes uniquement quand nécessaire.

### 2.3 Ce qui ne change PAS

Les usages de `millis()` pour des intervalles courts (quelques secondes à quelques minutes) dans les timers de ManagerUTC::handle() n'ont pas besoin d'être modifiés — la soustraction uint32_t gère correctement un seul débordement pour des intervalles < 49.7 jours. Mais par cohérence, on pourrait les migrer aussi.

---

## 3. Impact sur chaque fichier

### 3.1 VirtualClock.h / .cpp

**Variables :**
- `_anchorMillis` : `uint32_t` → `int64_t`, renommé `_anchorUs`

**Includes :**
- Ajouter `#include "esp_timer.h"`

**init() et sync() :**
- `millis()` → `esp_timer_get_time()`

**nowVirtual() :**
- `(millis() - _anchorMillis) / 1000UL` → `(esp_timer_get_time() - _anchorUs) / 1000000L`
- Le résultat est toujours un `time_t` (secondes) → pas d'impact sur les appelants

**Impact sur les appelants de nowVirtual() :** AUCUN. La signature ne change pas. Le retour est toujours un `time_t`.

### 3.2 ManagerUTC.h / .cpp

**readUTC() cas 3 :**
- Actuellement : `t.timestamp = static_cast<time_t>(millis())`
- `timestamp` est un `time_t` (secondes depuis une époque). Mettre du millis dedans est déjà une incohérence conceptuelle. Avec esp_timer ce serait encore pire (microsecondes dans un time_t).

**Question fondamentale :** Le cas 3 stocke du millis dans un champ prévu pour de l'UTC. C'est exploité ensuite pour :
- La réparation dans handle() : `millis() - record.timestamp`
- L'affichage "Depuis Xs" : `millis() - d.timestamp`

Ces calculs ont besoin d'un temps relatif au boot. Si on passe en esp_timer, il faut que ce soit cohérent : stocker et comparer avec la même base de temps.

**Proposition :** 
- Le cas 3 de readUTC() stocke `esp_timer_get_time() / 1000` (millisecondes 64 bits) dans timestamp
- Le champ `timestamp` dans TimeUTC, DataRecord, LastDataForWeb passe de `uint32_t`/`time_t` (32 bits) à `int64_t` (64 bits)
- Tous les calculs de delta utilisent `esp_timer_get_time() / 1000` au lieu de `millis()`

### 3.3 Structs (DataLogger.h)

**TimeUTC :**
```
time_t timestamp → int64_t timestamp
```

**DataRecord :**
```
uint32_t timestamp → int64_t timestamp
```

**LastDataForWeb :**
```
time_t timestamp → int64_t timestamp
```

Note : `time_t` est déjà un `int64_t` sur ESP32-S3 avec ESP-IDF >= 5.0. À vérifier. Si c'est le cas, le changement est transparent pour les cas UTC (time_t = int64_t). Pour le cas millis, on passe de `uint32_t` (49.7 jours max) à `int64_t` (292 000 ans).

### 3.4 DataLogger.cpp

**push() :**
- Le timestamp vient de readUTC() → pas de changement dans push() lui-même, le type change dans la struct

**handle() réparation :**
- `millis() - record.timestamp` → `esp_timer_get_time() / 1000 - record.timestamp`
- Ou mieux : `int64_t nowMs = esp_timer_get_time() / 1000; int64_t deltaMs = nowMs - record.timestamp;`

**flushToFlash() :**
- `f.printf("%lu,...)` → le format `%lu` ne suffit plus pour un int64_t. Utiliser `%lld` ou convertir en String.
- Impact sur la taille des lignes CSV (un timestamp 64 bits peut faire plus de caractères)

**init(), getLastUtcRecord(), getGraphCsv() :**
- Parsing : `line.substring().toInt()` retourne un `long`. Pour un int64_t il faut `.toFloat()` puis cast, ou utiliser `strtoll()`.
- C'est un point technique à traiter avec soin.

### 3.5 PagePrincipale.cpp

**timeHtml() :**
- `millis() - d.timestamp` → `esp_timer_get_time() / 1000 - d.timestamp`
- Le résultat est un delta en millisecondes, utilisé pour "Depuis Xs"

### 3.6 WebServer.cpp

**buildBundleHeader() :**
- Le champ `csvColumns` est déjà générique (noms de colonnes). Pas de changement.
- Le format du timestamp dans le CSV change de taille mais pas de sémantique.

### 3.7 Format CSV SPIFFS

Le format reste 7 champs : `timestamp,UTC_available,UTC_reliable,type,id,valueType,value`

Mais `timestamp` passe de 10 chiffres max (uint32_t : 4 294 967 295) à potentiellement 13+ chiffres pour l'UTC (time_t 64 bits) et 13+ chiffres pour les millis esp_timer (après quelques mois).

**Incompatibilité avec l'historique existant :** NON si les valeurs UTC restent dans la plage uint32. Les timestamps UTC actuels (~1,7 milliard) tiennent dans un int64_t sans problème. Seul le parsing doit accepter des nombres plus grands.

**Prérequis :** suppression de l'historique avant déploiement (déjà prévu — changement de format CSV).

---

## 4. Points d'attention

### 4.1 Vérifier le type de time_t sur ESP32-S3

Si `time_t` est déjà `int64_t` (probable avec ESP-IDF >= 5.0), alors le champ `timestamp` dans TimeUTC peut rester `time_t` et les cas millis s'y intègrent naturellement (les millis esp_timer en int64 tiennent dans un time_t int64).

Si `time_t` est `int32_t` (anciennes versions), il faut utiliser `int64_t` explicitement.

**À vérifier en compilant avec un `static_assert(sizeof(time_t) == 8)` ou un `Serial.println(sizeof(time_t))`.**

### 4.2 Cohérence des unités

Actuellement, quand `UTC_available = true`, `timestamp` contient des secondes (époque Unix). Quand `UTC_available = false`, il contient des millisecondes (millis depuis le boot). Ce sont deux unités différentes dans le même champ.

C'est un compromis existant qu'on conserve. Le booléen `UTC_available` dit comment interpréter la valeur.

### 4.3 Performance de esp_timer_get_time()

L'appel a un "overhead modéré" selon la documentation Espressif. C'est un accès à un registre matériel 64 bits sur un processeur 32 bits, avec une protection contre les lectures incohérentes. Largement acceptable pour nos usages (quelques appels par seconde au maximum).

### 4.4 Usages de millis() conservés

Les timers courts dans ManagerUTC::handle() (`_lastTourMs`, `_lastAttemptMs`, `_networkUpSinceMs`) et dans les autres modules pourraient rester en `millis()` car les intervalles sont courts. Mais par cohérence, les migrer aussi évite toute confusion future.

**Décision à prendre :** migrer tout ou seulement les timestamps longue durée.

### 4.5 RTCManager

`RTCManager::read()` retourne un `time_t` via référence. Si `time_t` est `int64_t`, aucun changement. Si non, il faudrait adapter.

Le double ping utilise `delay(250)` qui est indépendant de millis().

---

## 5. Ordre de mise en œuvre proposé

1. **Vérifier sizeof(time_t)** sur l'ESP32-S3 avec le toolchain actuel
2. **VirtualClock** — migration interne (transparent pour les appelants)
3. **ManagerUTC::readUTC()** — cas 3 passe en esp_timer
4. **Structs** (DataLogger.h) — timestamp en int64_t si nécessaire
5. **DataLogger.cpp** — adaptation parsing et printf
6. **PagePrincipale.cpp** — adaptation timeHtml()
7. **Tests** — vérifier compilation, format CSV, affichage

---

## 6. Fichiers impactés

| Fichier | Impact |
|---------|--------|
| VirtualClock.h | _anchorMillis → _anchorUs (int64_t) |
| VirtualClock.cpp | esp_timer_get_time() au lieu de millis() |
| ManagerUTC.h | timestamp dans TimeUTC : type à vérifier |
| ManagerUTC.cpp | readUTC() cas 3, handle() timers éventuellement |
| DataLogger.h | timestamp dans DataRecord et LastDataForWeb |
| DataLogger.cpp | push(), handle(), flushToFlash(), init(), getLastUtcRecord(), getGraphCsv() |
| PagePrincipale.cpp | timeHtml() delta |
| WebServer.cpp | Aucun changement probable |
| TimingConfig.h | Aucun changement |
| RTCManager.h/.cpp | Aucun changement si time_t = int64_t |
| main.cpp | Aucun changement |
