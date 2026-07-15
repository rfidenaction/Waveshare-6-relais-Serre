# Synthèse technique — RS485 & Capteur sol ZTS-3000-TR-WS-N01

*Document de référence pour les développements RS485 du projet serre*
*Date : mai 2026*

---

## 1. Interface RS485 — Waveshare ESP32-S3-Relay-6CH

### Pins UART

| Fonction | GPIO | Définition IO-Config.h |
|----------|------|------------------------|
| TX (UART → RS485) | GPIO 17 | `RS485_TX_PIN` |
| RX (RS485 → UART) | GPIO 18 | `RS485_RX_PIN` |

Ces pins sont sur **UART1** de l'ESP32-S3. UART0 est utilisé par le USB-to-UART (console série).

### Chaîne matérielle (du MCU vers le bornier)

```
ESP32-S3 (3.3V)  →  π141E61 (isolateur numérique)  →  SP485EEN (transceiver RS485, 5V isolé)  →  Bornier A/B
       TXD  ──────────►  TXD'
       RXD  ◄──────────  RXD'
       TXD1EN ────────►  TXDEN'  ──►  DE (pin 3 du SP485EEN)
```

### Contrôle de direction

**Automatique par hardware.** Le signal TXD1EN bascule la ligne DE du SP485EEN
via l'isolateur π141E61. Aucun GPIO de direction (DE/RE) à gérer côté logiciel.
En pratique : on utilise Serial1 normalement — écrire = émettre, lire = recevoir.

### Alimentation isolée

Le côté RS485 est alimenté par un **B0505S-3WR2** (DC-DC isolé 5V → 5V).
Isolation galvanique complète entre le domaine ESP32 et le bus RS485.

### Protection

- TVS : SMAJ6.5CA, SMAJ12CA (surge + ESD)
- Fusibles réarmables : BSMD1206-050
- Diode SM712 (clamping différentiel A/B)

### Résistance de terminaison 120Ω

Réservée sur la carte, **non connectée par défaut**. Activable via un **jumper** (position « 120R »).
À activer si la carte est en bout de ligne du bus RS485.

### LEDs indicatrices

- **Vert** : données en émission (TX)
- **Bleu** : données en réception (RX)

---

## 2. Capteur ZTS-3000-TR-WS-N01 — Liyuan Electronic

### Nature du capteur

Sonde de **sol** (soil probe) mesurant :
- **Humidité du sol** (volume moisture, 0–100%)
- **Température du sol** (-40°C à +80°C)

Forme : sonde à insérer dans le substrat, dimensions ~123×45×15 mm (hors câble).
Étanchéité IP68, câble standard 2 m (extensible jusqu'à 1200 m).

### Décodage du modèle

```
ZTS  : code fabricant (Liyuan / marque générique)
3000 : gamme produit
TR   : boîtier sonde sol (soil testing housing)
WS   : WenShi (温湿) = Température + Humidité (moisture)
N01  : sortie RS485 Modbus-RTU
```

### Protocole Modbus RTU — Paramètres par défaut

| Paramètre      | Valeur         |
|-----------------|----------------|
| Baud rate       | **4800**       |
| Parité          | Aucune (N)     |
| Bits de données | 8              |
| Bit de stop     | 1              |
| Adresse esclave | **0x01**       |
| Code fonction   | **0x03** (Read Holding Registers) |

### Carte des registres

| Adresse | Contenu                | Accès     | Notes |
|---------|------------------------|-----------|-------|
| 0x0000  | Humidité sol           | Lecture   | Valeur réelle × 10 (ex: 658 = 65,8%) |
| 0x0001  | Température sol        | Lecture   | Valeur réelle × 10, complément à deux si < 0°C |
| 0x07D0  | Adresse esclave        | Lecture/Écriture | 1–254 (défaut : 1) |
| 0x07D1  | Baud rate              | Lecture/Écriture | 0=2400, 1=4800, 2=9600 |

### Trame de requête (lecture T° + humidité, adresse 1)

```
TX : 01 03 00 00 00 02 C4 0B
      │  │  │     │     └── CRC16 (low, high)
      │  │  │     └── Nombre de registres : 2
      │  │  └── Registre de départ : 0x0000
      │  └── Code fonction : 0x03
      └── Adresse esclave : 0x01
```

### Trame de réponse attendue (7 octets utiles + 2 CRC)

```
RX : 01 03 04 XX XX YY YY CC CC
      │  │  │  │     │     └── CRC16 (low, high)
      │  │  │  │     └── Température (high, low) — valeur × 10
      │  │  │  └── Humidité (high, low) — valeur × 10
      │  │  └── Nombre d'octets de données : 4
      │  └── Code fonction : 0x03
      └── Adresse esclave : 0x01
```

### Décodage de la température

- **T ≥ 0°C** : valeur directe ÷ 10. Ex : 0x00FE = 254 → 25,4°C
- **T < 0°C** : complément à deux. Ex : 0xFF9B = 65435 (uint16) → interprété signé = -101 → -10,1°C

En code C++ : `float tempC = (int16_t)rawValue / 10.0f;` — le cast en int16_t gère automatiquement le complément à deux.

### Décodage de l'humidité

Valeur directe ÷ 10. Ex : 0x0292 = 658 → 65,8%

---

## 3. Gestion multi-capteurs (3 sondes identiques)

### Principe

Modbus RTU identifie chaque esclave par son **adresse** (1–254) sur un bus partagé
(câblage en série/daisy-chain sur les mêmes fils A et B).

### Procédure de configuration des adresses

Les capteurs sortent d'usine avec l'adresse 0x01. Il faut les reconfigurer **un par un** :

1. Brancher **un seul** capteur sur le bus RS485
2. Envoyer une trame d'écriture registre (fonction 0x06) sur le registre 0x07D0
   avec la nouvelle adresse souhaitée
3. Le capteur répond avec la même trame (echo) — confirmation
4. Débrancher, passer au capteur suivant

**Trame de changement d'adresse** (ex : adresse 1 → adresse 2) :

```
TX : 01 06 07 D0 00 02 08 86
      │  │  │     │     └── CRC16
      │  │  │     └── Nouvelle adresse : 0x0002
      │  │  └── Registre : 0x07D0 (adresse esclave)
      │  └── Code fonction : 0x06 (Write Single Register)
      └── Adresse actuelle : 0x01
```

**Astuce** : si l'adresse actuelle est inconnue, utiliser l'adresse broadcast **0xFF**
(un seul capteur branché obligatoirement).

### Convention d'adressage prévue

| Adresse Modbus | Capteur         | Emplacement (à définir) |
|----------------|-----------------|-------------------------|
| 1              | Sonde sol n°1   | —                       |
| 2              | Sonde sol n°2   | —                       |
| 3              | Sonde sol n°3   | —                       |

---

## 4. Contraintes d'implémentation logicielle

### Timing Modbus RTU

Le protocole Modbus RTU utilise le silence sur la ligne pour délimiter les trames.
Un silence ≥ 3,5 caractères marque la fin d'une trame.
À 4800 bauds (11 bits/caractère) : 3,5 × 11 / 4800 ≈ **8 ms** de silence inter-trame.

### Séquence logicielle pour une lecture

1. Vider le buffer RX (`Serial1.flush()` + purge des octets résiduels)
2. Envoyer la trame de requête (8 octets)
3. Attendre la réponse (timeout ~200 ms recommandé)
4. Lire les octets reçus (9 attendus : 1+1+1+4+2)
5. Valider le CRC16
6. Décoder les registres

### Non-bloquant

Le système utilise un TaskManager coopératif. La lecture RS485 doit être
**non-bloquante** : machine d'états (envoi → attente → lecture → décodage)
plutôt qu'un délai bloquant.

### CRC16 Modbus

Polynôme : 0xA001 (bit-reversed 0x8005). Valeur initiale : 0xFFFF.
Calcul sur tous les octets sauf les 2 derniers (qui portent le CRC).
Ordre dans la trame : octet bas en premier, octet haut ensuite.

---

## 5. Câblage physique

### Couleurs des fils du capteur ZTS-3000-TR-WS-N01 (Liyuan Electronic)

| Fil      | Couleur   | Fonction          |
|----------|-----------|-------------------|
| Marron   | 🟫 Brown  | Power positive (V+ : 4,5–30V DC) |
| Noir     | ⬛ Black  | Power negative (GND) |
| Bleu     | 🟦 Blue   | 485-B (−)         |
| Jaune    | 🟨 Yellow | 485-A (+)         |

### Raccordement au bornier RS485 de la carte

```
ESP32-S3-Relay-6CH             Capteur ZTS-3000
    bornier RS485                fil du câble
  ┌──────────────┐            ┌──────────────────┐
  │     A (+)    │────────────│  Jaune  (485-A)  │
  │     B (-)    │────────────│  Bleu   (485-B)  │
  └──────────────┘            └──────────────────┘

  Alimentation séparée :
  ┌──────────────┐            ┌──────────────────┐
  │     V+       │────────────│  Marron (V+)     │
  │     GND      │────────────│  Noir   (GND)    │
  └──────────────┘            └──────────────────┘
```

### Bus multi-capteurs (3 sondes)

Pour 3 capteurs : câblage en **daisy-chain** (A-A-A, B-B-B, GND commun, V+ commun).
Résistance 120Ω en bout de ligne (jumper sur la carte + éventuellement
au niveau du dernier capteur si celui-ci n'en a pas intégré).

---

*Ce document sert de référence. Les valeurs par défaut (adresse, baud rate)
correspondent à la documentation constructeur de la famille ZTS-3000/3001.
À vérifier empiriquement lors du premier test avec le capteur réel.*
