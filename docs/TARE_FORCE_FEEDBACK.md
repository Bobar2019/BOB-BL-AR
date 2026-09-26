# Tare par retour de force — HID Output Report (Rumble)

> Réinitialisation de l'origine de la position tête (`pitch = 0`, `roll = 0`,
> `yaw = 0`) déclenchée depuis le poste de pilotage **sans passer par le
> Wi-Fi**, en exploitant le canal natif HID **Output Report** (retour de
> force / rumble) de la manette BLE.

---

## Table des matières

1. [Principe de fonctionnement](#1-principe-de-fonctionnement)
2. [Structure du paquet HID](#2-structure-du-paquet-hid)
3. [Exemple Frontend — JavaScript / Web Gamepad API](#3-exemple-frontend--javascript--web-gamepad-api)
4. [Exemple Backend — Python (hidapi / pygame)](#4-exemple-backend--python-hidapi--pygame)
5. [Comportement détaillé du firmware](#5-comportement-détaillé-du-firmware)
6. [Procédure de validation](#6-procédure-de-validation)
7. [Limites connues](#7-limites-connues)

---

## 1. Principe de fonctionnement

### 1.1 Pourquoi l'Output Report évite la coexistence radio Wi-Fi / BLE

L'ESP32-C6 ne possède qu'**une seule radio** partagée entre le Wi-Fi (AP +
STA) et le Bluetooth LE. Leur coexistence repose sur un arbitrage temporel
(time-slicing) du front radio : chaque activité Wi-Fi — balise du point
d'accès, télémétrie WebSocket à 10 Hz, portail captif, scans — interrompt
périodiquement les connexions BLE. À l'inverse, le profil HID vit sur des
intervalles de connexion de 7,5 à 15 ms : toute latence radio se paie
directement en gigue sur les axes de la manette.

Commander la tare par le canal Wi-Fi imposerait en outre :

- une **association** au point d'accès « BOB BL AR » (ou au réseau STA) ;
- la traversée complète du **portail captif**, du DNS wildcard, du TCP et du
  HTTP (`POST /api/gamepad/tare`) ;
- une **pile réseau active en permanence** alors que le poste de pilotage
  est souvent hors de portée du Wi-Fi (typiquement : un jeu sur un PC fixe
  relié à la manette en Bluetooth seul).

### 1.2 La solution : le canal de retour de force du profil HID

Le profil HID-over-GATT (service `0x1812`) définit **trois directions de
rapports** :

| Direction | Sens | Usage dans BOB BL AR |
|---|---|---|
| Input Report n°1 | manette → hôte | sticks (X, Y, Z, Rz) + boutons, en notification à ~66 Hz |
| Input Reports n°2/4 | manette → hôte | AC Home, batterie — déclarés, jamais notifiés (exigés par le protocole Xbox) |
| **Output Report n°3** | **hôte → manette** | **rumble « Set Effect » — canal de commande de la Tare** |

L'Output Report est le canal **standard** du retour de force : c'est lui que
les manettes du commerce utilisent pour leurs moteurs de vibration, et il
**existe dès l'appairage**. Aucune nouvelle connexion, aucune pile
supplémentaire, aucune contention radio supplémentaire : l'ordre de rumble
emprunte la connexion BLE déjà établie, dans un créneau de connexion déjà
réservé par le protocole HID.

```
Poste de pilotage (hôte)
  navigateur web / script Python
        │  playEffect('dual-rumble')  ·  hid_write(...)
        ▼
  pile manette de l'OS (GCController / pilote Xbox / hid-microsoft)
        │  Write GATT → caractéristique « Output Report »
        │  (Report Reference n°3, type 0x02)
        ▼
┌───────────────────────── ESP32-C6 ─────────────────────────┐
│  NimBLE · BleGamepadHid::OutputCallbacks::onWrite()        │
│    parse « Set Effect » (8 octets, page PID)               │
│    magnitude non nulle → anti-rebond 250 ms                │
        ▼                                                    │
│  handler (main.cpp) → HeadTracker::requestTare()           │
│    simple drapeau — contexte de callback allégé            │
        ▼                                                    │
│  tâche manette 100 Hz → HeadTracker::tare()                 │
│    _tarePitch = _rawPitchDeg · _tareRoll = … · _tareYaw = … │
│    offsets persistés en NVS (tarP / tarR / tarY)           │
        ▼                                                    │
│  Input Report n°1 suivant : angles recentrés (0, 0, 0)      │
└─────────────────────────────────────────────────────────────┘
```

### 1.3 Pourquoi une identité « Xbox One S » : comment l'hôte décide d'exposer `vibrationActuator`

**Le point clé (vérifié dans le code source de Chromium)** : un navigateur
ne scanne **pas** le descripteur HID à la recherche d'une collection
« dual-rumble » générique pour instancier `gamepad.vibrationActuator`. La
décision est prise en amont par la pile manette de l'OS, qui ne lie son
pilote haptique qu'aux périphériques qu'elle sait piloter :

| OS | Voie rumble de la pile | Périphériques reconnus |
|---|---|---|
| macOS 14+ (Sonoma) | GCController `.haptics` (CoreHaptics) | manettes Xbox / PlayStation / MFi de sa base |
| Windows 10 1809+ | pilote Xbox → Windows.Gaming.Input (vibration native pour les apps/jeux et Chrome) | VID `0x045E` |
| Linux | `hid-microsoft` (HID) → evdev `EV_FF`/`FF_RUMBLE` | `0x045E:0x02FD` depuis le noyau 4.15 |

Chromium s'appuie sur ces piles : sur macOS, `vibrationActuator` n'est
instancié que si `GCController.haptics` existe ; sous Windows, les manettes
Xbox remontent par Windows.Gaming.Input avec leur vibration native ; sous
Linux, il faut un pilote evdev avec force-feedback. La contre-preuve est
dans `device/gamepad/hid_haptic_gamepad.cc` : la voie « HID générique » de
Chromium ne matche que **trois VID/PID** au monde (Stadia `0x18d1:0x9400`
et prototype `0x6666:0x9401`, adaptateur XSkills `0x0b43:0x0005`). Un VID
communautaire pid.codes comme l'ancien `0x1209:0xB0B1` laissait la manette
« générique » : axes lisibles, mais **TARE INDISPONIBLE** — le constat qui
a motivé cette refonte.

La solution firmware est donc une **émulation Xbox One S (modèle 1708)** —
la seule voie vers un vibreur « natif » reconnu par les trois OS sans WebHID
ni HTTPS :

- **identité PnP** : VID `0x045E` (Microsoft), PID `0x02FD` (Xbox One S),
  manufacturer « Microsoft », numéro de série d'une manette réelle
  (caractéristique 0x2A25) — `include/config.h` ;
- **descripteur HID du protocole Xbox**, transcription octet pour octet
  d'une capture de manette réelle (Mystfit/ESP32-BLE-CompositeHID, reprise
  et éprouvée en production par ESP32-BLE-Gamepad de lemmingDev) :
  rapport d'entrée n°1 de 16 octets — c'est lui qui donne
  `mapping: "standard"` dans Chrome — et Output Report n°3 « Set Effect »
  de la page d'usages PID (0x0F), exactement la trame que les piles Xbox
  écrivent pour traduire `playEffect('dual-rumble', …)`.

Les axes du MPU-9250 restent injectés sur les sticks analogiques : roulis →
X, tangage → Y (stick gauche), lacet → Z (stick droit), le jerk reste un
bouton (A). Le nom Bluetooth (NVS, personnalisable) reste « BOB BL AR » :
l'identité Xbox repose sur le triplet VID/PID + descripteur, pas sur le nom.

### 1.4 La page PID (0x0F) : le strict nécessaire du protocole Xbox

La spécification « Physical Interface Device » (page d'usages `0x0F`) définit
un protocole complet de retour de force (Set Effect, Effect Operation, PID
Block Load, Free Block…). Le descripteur n'en déclare **que le sous-ensemble
utilisé par le protocole Xbox** : le *Set Effect Report* (usage 0x21) avec
DC Enable Actuators, quatre Magnitude (gâchettes + moteurs), Duration, Start
Delay et Loop Count. C'est volontairement exact — octet pour octet — car les
pilotes Xbox des hôtes exigent ce format précis ; en retour, ils routent
eux-mêmes le rumble hôte → manette sans aucun dialogue PID supplémentaire
(le rapport est « fire and forget » : écrit, jamais acquitté).

---

## 2. Structure du paquet HID

### 2.1 Descripteur — les quatre rapports déclarés

| Report ID | Direction | Taille | Contenu | Caractéristique GATT |
|---|---|---|---|---|
| 0x01 | manette → hôte | 16 octets | sticks u16 + gâchettes + D-pad + 15 boutons + Share | Report Reference `{0x01, 0x01}` — notification |
| 0x02 | manette → hôte | 1 octet | AC Home — jamais notifié | Report Reference `{0x02, 0x01}` — **doit exister, sinon Windows rejette le service** |
| 0x03 | **hôte → manette** | 8 octets | **rumble « Set Effect » — canal de la Tare** | Report Reference `{0x03, 0x02}` — écriture (Write + Write Without Response) |
| 0x04 | manette → hôte | 1 octet | Battery Strength — initialisé (100), jamais notifié | Report Reference `{0x04, 0x01}` |

### 2.2 Rapport d'entrée n°1 (16 octets)

| Octets | Champ | Type | Signification |
|:---:|-------|------|---------------|
| 0-1 | `x` | u16 LE | stick gauche X — **roulis** (`0x8000` = centré) |
| 2-3 | `y` | u16 LE | stick gauche Y — **tangage** |
| 4-5 | `z` | u16 LE | stick droit X — **lacet** |
| 6-7 | `rz` | u16 LE | stick droit Y — réservé (maintenu `0x8000`, **pas 0** : 0 = butée basse) |
| 8-9 | `brake` | 10 bits + 6 bits de bourrage | gâchette gauche (0 = relâchée) |
| 10-11 | `accelerator` | 10 bits + 6 bits de bourrage | gâchette droite (0 = relâchée) |
| 12 | `hat` | 4 bits + 4 bits de bourrage | D-pad : 0 = neutre, 1..8 = Nord puis horaire |
| 13-14 | `buttons` | 15 bits + 1 bit de bourrage | bit 0 (`0x0001`) = **A = jerk** (bit 1 = B, bit 3 = X, bit 4 = Y, …) |
| 15 | `share` | 1 bit + 7 bits de bourrage | bouton Share/Record (0) |

C'est ce layout — sticks `X/Y/Z/Rz` et boutons à leur place canonique — qui
fait reconnaître le `mapping: "standard"` de la Web Gamepad API : axes 0/1 =
stick gauche, axes 2/3 = stick droit, bouton 0 = A.

### 2.3 Output Report n°3 (8 octets) — « Set Effect Report » (page PID)

| Octet | Champ | Plage | Signification |
|:---:|-------|-------|---------------|
| 0 | `dcEnableActuators` | 4 bits (+4 de bourrage) | actionneurs activés (les 4 « moteurs ») |
| 1 | `leftTriggerMagnitude` | 0..100 | moteur de gâchette gauche |
| 2 | `rightTriggerMagnitude` | 0..100 | moteur de gâchette droite |
| 3 | `weakMagnitude` | 0..100 | moteur faible — **non nul = ordre de Tare** |
| 4 | `strongMagnitude` | 0..100 | moteur fort — **non nul = ordre de Tare** |
| 5 | `duration` | 0..255 | durée en unités de **10 ms** (0 = infini) |
| 6 | `startDelay` | 0..255 | délai avant effet, unités de 10 ms |
| 7 | `loopCount` | 0..255 | répétitions (0 = infini) |

Règles de décodage côté ESP32-C6 (`OutputCallbacks::onWrite`) :

- **toute magnitude non nulle** (moteurs fort/faible **ou** gâchettes —
  certains effets de l'hôte, notamment via GCController, ne sollicitent
  qu'elles) déclenche la tare — le ou les « moteurs » sollicités n'ont pas
  d'importance ;
- un rapport aux quatre magnitudes **nulles** est un ordre d'arrêt de
  vibration : ignoré ;
- **anti-rebond de 250 ms** (`BLE_RUMBLE_DEBOUNCE_MS`, `config.h`) : un
  effet continu relancé périodiquement par l'hôte ne déclenche qu'une seule
  tare par fenêtre (protège la NVS et la trace série) ;
- selon la pile hôte, l'octet de **Report ID `0x03` préfixe** la valeur
  écrite dans la caractéristique GATT (usage de la spécification HID
  Service) — le firmware accepte **les deux formats** : 8 octets nus, ou
  9 octets avec préfixe Report ID.

### 2.4 Exemples de trames valides

```
03 0F 00 00 64 64 0A 00 00   → Report ID 3 : enables 0x0F, faible=100, fort=100, durée=100 ms → TARE
0F 00 00 64 32 0A 00 00      → sans préfixe  : faible=100, fort=50, durée=100 ms               → TARE
0F 00 00 00 00 00 00 00      → arrêt de vibration                                           → ignoré
```

La première trame est représentative de ce qu'émet la pile Xbox de l'hôte
après `playEffect('dual-rumble', { duration: 100, strongMagnitude: 1.0,
weakMagnitude: 1.0 })` : enables `0x0F`, magnitudes converties en
pourcentage (0..100), durée arrondie en unités de 10 ms.

---

## 3. Exemple Frontend — JavaScript / Web Gamepad API

### 3.1 Détection du gamepad et tare par la voie native

Avec l'identité Xbox, `gamepad.vibrationActuator` est instancié par Chrome
sans WebHID ni HTTPS — `playEffect('dual-rumble')` atteint l'Output Report
n°3 via la pile manette de l'OS.

```html
<!DOCTYPE html>
<html lang="fr">
<head><meta charset="utf-8"><title>BOB BL AR — Tare par rumble</title></head>
<body>
  <p>Appuyez sur la touche <kbd>T</kbd> pour recentrer le point 0 de la tête.</p>
  <p id="status">Recherche de la manette…</p>

<script>
'use strict';

/* BOB BL AR s'annonce avec l'identité Xbox 0x045E:0x02FD : le gamepad.id
   contient « Vendor: 045e Product: 02fd » (certains hôtes y mettent aussi
   le nom Bluetooth personnalisé). */
function isBob(pad) {
  const id = (pad && pad.id ? pad.id : '').toUpperCase();
  return id.includes('045E') || id.includes('BOB BL AR');
}

let bobGamepad = null;

window.addEventListener('gamepadconnected', (e) => {
  if (isBob(e.gamepad)) {
    bobGamepad = e.gamepad;
    document.getElementById('status').textContent =
      'Manette détectée : ' + e.gamepad.id +
      (e.gamepad.mapping === 'standard' ? ' (mapping standard)' : '');
  }
});

window.addEventListener('gamepaddisconnected', (e) => {
  if (e.gamepad === bobGamepad) {
    bobGamepad = null;
    document.getElementById('status').textContent = 'Manette déconnectée.';
  }
});

/* Les connexions antérieures au chargement de la page sont pollées : */
function poll() {
  if (!bobGamepad) {
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    for (const pad of pads) {
      if (isBob(pad)) {
        bobGamepad = pad;
        document.getElementById('status').textContent =
          'Manette détectée : ' + pad.id;
        break;
      }
    }
  }
  requestAnimationFrame(poll);
}
requestAnimationFrame(poll);

/* --- Déclenchement de l'effet haptique = ordre de Tare -------------- */
window.addEventListener('keydown', async (e) => {
  if (e.key !== 't' && e.key !== 'T') return;
  if (!bobGamepad) return;

  const actuator = bobGamepad.vibrationActuator;
  if (!actuator) {
    document.getElementById('status').textContent =
      'vibrationActuator indisponible (OS trop ancien ? cf. doc §7)';
    return;
  }

  /* Un ordre de rumble non nul → l'ESP32-C6 enregistre les angles
     actuels comme nouvelle référence neutre (pitch = roll = yaw = 0). */
  await actuator.playEffect('dual-rumble', {
    duration:        100,    // ms (informative côté firmware)
    strongMagnitude: 1.0,    // moteur fort  → 0..100 sur le fil
    weakMagnitude:   1.0     // moteur faible → 0..100 sur le fil
  });
  document.getElementById('status').textContent = 'Tare envoyée (rumble).';
});
</script>
</body>
</html>
```

> **Conditions de fonctionnement** : la voie native exige que la pile de
> l'OS reconnaisse la manette comme une manette Xbox (c'est le rôle de
> l'identité 0x045E:0x02FD + descripteur du protocole, cf. §1.3) :
> macOS 14+, Windows 10 1809+, Linux ≥ 4.15 avec `hid-microsoft`. Sur un
> hôte plus ancien, utiliser la **variante WebHID** ci-dessous, qui écrit
> directement l'Output Report et fonctionne systématiquement.

### 3.2 Variante de repli — WebHID (Chrome / Edge)

```js
'use strict';

const FILTERS = [{ vendorId: 0x045E, productId: 0x02FD }];  // BOB BL AR (identité Xbox One S)

async function tareByWebHID(strong = 100, weak = 100, duration10ms = 10) {
  /* Nécessite un geste utilisateur (clic) et HTTPS ou localhost. */
  const [device] = await navigator.hid.requestDevice({ filters: FILTERS });
  if (!device) throw new Error('Aucun appareil sélectionné');

  await device.open();
  try {
    /* sendReport(reportId, payload) — le navigateur achemine via la pile
       HID de l'OS : Report ID 3 + « Set Effect » Xbox (8 octets).
       duration10ms : durée en unités de 10 ms (10 → 100 ms). */
    await device.sendReport(3, new Uint8Array([
      0x0F,          // DC Enable Actuators : les 4 moteurs
      0x00,          // gâchette gauche
      0x00,          // gâchette droite
      weak,          // moteur faible (0..100)
      strong,        // moteur fort   (0..100)
      duration10ms,  // durée (unités de 10 ms)
      0x00,          // délai
      0x00           // répétitions
    ]));
  } finally {
    await device.close();
  }
}

document.getElementById('tareBtn').addEventListener('click',
  () => tareByWebHID().catch(console.error));
```

---

## 4. Exemple Backend — Python (hidapi / pygame)

### 4.1 hidapi (recommandé — universel)

```python
#!/usr/bin/env python3
"""BOB BL AR — tare de la position tête par Output Report HID (rumble).

Prérequis : manette appairée au système (Bluetooth), puis :
    pip install hid
"""
import hid

VID, PID = 0x045E, 0x02FD        # BOB BL AR (identité Xbox One S)
REPORT_ID = 0x03                 # Output Report « Set Effect » (page PID)


def find_device():
    """Retourne le chemin hidapi de la manette BOB BL AR."""
    devices = hid.enumerate(VID, PID)
    if not devices:
        raise RuntimeError(
            "BOB BL AR introuvable — appairer la manette au préalable")
    return devices[0]['path']


def tare(path, strong=100, weak=100, duration_10ms=10):
    """Envoie l'ordre de rumble : toute magnitude non nulle re-tare la tête.

    hid_write attend le Report ID en premier octet, suivi de la charge
    utile « Set Effect » Xbox : enables | gâchette G | gâchette D |
    faible | fort | durée (10 ms) | délai | boucle.
    """
    h = hid.device()
    h.open_path(path)
    try:
        buf = [REPORT_ID,
               0x0F,            # DC Enable Actuators (4 moteurs)
               0x00, 0x00,      # gâchettes gauche/droite
               weak, strong,    # moteurs faible / fort (0..100)
               duration_10ms,   # durée en unités de 10 ms
               0x00, 0x00]      # délai, répétitions
        if h.write(buf) <= 0:
            raise RuntimeError("Écriture de l'Output Report impossible")
    finally:
        h.close()


if __name__ == '__main__':
    tare(find_device())
    print("Ordre de tare envoyé (rumble HID).")
```

### 4.2 pygame / SDL

`pygame` expose `Joystick.rumble(low_frequency, high_frequency, duration)` :

```python
import pygame

pygame.init()
pygame.joystick.init()
for i in range(pygame.joystick.get_count()):
    stick = pygame.joystick.Joystick(i)
    if '045e' in stick.get_guid() or 'BOB BL AR' in stick.get_name().upper():
        stick.init()
        stick.rumble(1.0, 1.0, 100)   # → ordre de Tare côté ESP32-C6
        break
```

> **Note** : avec l'identité Xbox, la base de mappings de SDL connaît la
> manette — `rumble()` transite par le pilote Xbox de l'OS et fonctionne.
> hidapi (§ 4.1) reste la voie la plus directe (écriture de l'Output Report
> quelle que soit la plateforme).

---

## 5. Comportement détaillé du firmware

### 5.1 Correspondance des fonctions et variables

| Concept du cahier des charges | Implémentation firmware |
|---|---|
| `tare_origin()` | [`HeadTracker::tare()`](../src/head_tracker.cpp) — copie `_rawPitchDeg/_rawRollDeg/_rawYawDeg` dans `_tarePitch/_tareRoll/_tareYaw`, persiste en NVS (clés `tarP`, `tarR`, `tarY`) |
| `offset_pitch/roll/yaw` | `_tarePitch`, `_tareRoll`, `_tareYaw` (soustraits des angles bruts à chaque itération 100 Hz) |
| `current_pitch/roll/yaw` | `_rawPitchDeg`, `_rawRollDeg`, `_rawYawDeg` (volatiles, écrits par la tâche 100 Hz) |
| Callback `onWrite` HID | [`BleGamepadHid::OutputCallbacks::onWrite`](../src/ble_gamepad.cpp) — parse le rapport 8 octets, filtre, anti-rebond, notifie |
| Déclenchement `strongMagnitude > 0` | filtrage dans `onWrite`, politique câblée dans `main.cpp` (`_onRumbleOrder` → `requestTare()`) |
| Recentrage immédiat de l'Input Report | consommation du drapeau `_tareRequested` en tête de `_taskLoop()` : le rapport suivant part des angles recentrés (≤ 10 ms + intervalle de notification `BLE_SEND_MIN_INTERVAL_MS`) |

### 5.2 Choix d'architecture : callback léger + exécution différée

Le callback `onWrite` s'exécute dans la **tâche hôte NimBLE** (pile
limitée). Conformément à la convention du socle — jamais d'appel lourd dans
un callback — il se contente de :

1. décoder la trame (8 octets, avec ou sans préfixe Report ID) ;
2. écarter les ordres nuls et appliquer l'anti-rebond ;
3. tracer sur le port série ;
4. positionner le drapeau `_tareRequested` via le handler enregistré.

La tare complète — copie des offsets et **écriture NVS** — est exécutée par
la **tâche manette 100 Hz** dans son propre contexte. Ce circuit différé
existait déjà pour l'API REST (`POST /api/gamepad/tare` reste fonctionnel) ;
les deux canaux convergent vers la même fonction `tare()`.

### 5.3 Fichiers modifiés (refonte « identité Xbox »)

| Fichier | Modification |
|---|---|
| `include/config.h` | `BLE_VID`/`BLE_PID` → 0x045E/0x02FD, `BLE_MANUFACTURER` « Microsoft », `BLE_SERIAL_NUMBER`, `GAMEPAD_RUMBLE_REPORT_ID` (0x03) |
| `include/ble_gamepad.h` | `HidGamepadReport` 16 octets (layout Xbox, struct packée + static_assert), constantes `XBOX_BTN_*`/`XBOX_AXIS_CENTER` |
| `src/ble_gamepad.cpp` | Descripteur HID Xbox One S 1708 (4 rapports), PnP/serial Xbox dans `begin()`, `onWrite` adapté au rapport 8 octets, `sendReport` 16 octets |
| `src/head_tracker.cpp` | Conversion axes ±127 → u16 centré 0x8000 (`axisToXboxU16`), jerk → bouton A, rafale de rapports pendant les 2 s suivant la connexion |
| `include/head_tracker.h` | `requestTare()`, drapeau `_tareRequested` (inchangé) |
| `src/main.cpp` | handler `_onRumbleOrder` + enregistrement (inchangé) |

---

## 6. Procédure de validation

1. Compiler et flasher : `pio run -t upload` ;
2. **supprimer le bond précédent** sur l'hôte (réglages Bluetooth → oublier
   « BOB BL AR ») puis ré-appairer : l'identité (VID/PID, descripteur) a
   changé, l'hôte garde sinon l'ancienne manette « générique » en cache ;
3. vérifier dans l'hôte que la manette apparaît comme une **manette Xbox**
   (icône/mention « Xbox Wireless Controller » ou similaire) ;
4. dans Chrome (about:blank suffit, pas de HTTPS requis) :

```js
const pad = [...navigator.getGamepads()].find(p => p && p.id.toUpperCase().includes('045E'));
console.log(pad.id, pad.mapping);              // → "standard"
console.log(pad.vibrationActuator);            // → GamepadHapticActuator {type: "dual-rumble"}
await pad.vibrationActuator.playEffect('dual-rumble',
    {duration: 100, strongMagnitude: 1.0, weakMagnitude: 1.0});
```

5. incliner la tête : les axes 0/1 (stick gauche) et 2 (stick droit)
   bougent dans le testeur de manette de l'OS ou la télémétrie WebSocket ;
6. contrôler la trace série (115200 bauds) :

```
[BLE] Ordre de rumble : fort=100, faible=100, durée=100 ms
[TRACK] Tare effectué (offsets -12.3, 4.5, 87.6°)
```

7. les axes reviennent à ~0 alors que la tête est restée inclinée ;
8. redémarrer l'ESP32 : les offsets sont conservés (NVS `tarP/tarR/tarY`).

---

## 7. Limites connues

- **conditions OS de la voie native** : macOS 14+ (GCController haptics),
  Windows 10 1809+ (pilote Xbox), Linux ≥ 4.15 (`hid-microsoft`). En deçà,
  les axes restent lisibles mais `vibrationActuator` reste absent —
  utiliser WebHID (§ 3.2) ou hidapi (§ 4.1) ;
- **ré-appairage obligatoire** après cette refonte : l'identité
  (VID/PID + descripteur) a changé, les hôtes gardent l'ancien périphérique
  en cache de bonding ;
- **sémantique du canal** : tout rumble émis par un jeu (dégâts, collision…)
  re-tare la position — c'est le comportement voulu (« toute magnitude non
  nulle = ordre de recentrage »), atténué par l'anti-rebond de 250 ms ;
- **aucune vibration physique** : la manette ne comporte pas de moteurs,
  l'Output Report est un canal de commande pur ;
- le canal est **monodirectionnel hôte → manette** et sans accusé de
  réception : la confirmation se lit sur la télémétrie (axes recentrés) ou
  la trace série ;
- l'anti-rebond de 250 ms borne la cadence de tare à 4/s depuis le canal
  rumble (convergence REST/rumble inchangée côté `tare()`).
