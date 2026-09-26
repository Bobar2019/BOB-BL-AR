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
| Input Report | manette → hôte | axes (X, Y, Z, Rz) + boutons, en notification à ~66 Hz |
| **Output Report** | **hôte → manette** | **retour de force / rumble — canal de commande de la Tare** |
| Feature Report | bidirectionnel | non utilisé |

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
  pile HID de l'OS (macOS / Windows / Linux)
        │  Write GATT → caractéristique « Output Report »
        │  (Report Reference n°1, type 0x02)
        ▼
┌───────────────────────── ESP32-C6 ─────────────────────────┐
│  NimBLE · BleGamepadHid::OutputCallbacks::onWrite()        │
│    parse {strongMagnitude, weakMagnitude, durée}           │
│    magnitude non nulle → anti-rebond 250 ms                │
│        ▼                                                    │
│  handler (main.cpp) → HeadTracker::requestTare()           │
│    simple drapeau — contexte de callback allégé            │
│        ▼                                                    │
│  tâche manette 100 Hz → HeadTracker::tare()                 │
│    _tarePitch = _rawPitchDeg · _tareRoll = … · _tareYaw = … │
│    offsets persistés en NVS (tarP / tarR / tarY)           │
│        ▼                                                    │
│  Input Report suivant : angles recentrés (0, 0, 0)          │
└─────────────────────────────────────────────────────────────┘
```

### 1.3 Pourquoi pas la page d'usages PID (0x0F) complète

La spécification « Physical Input Device » (page d'usages `0x0F`) définit un
protocole complet de retour de force (Set Effect, Effect Operation, PID
Block Load, Free Block…). L'implémenter n'apporterait **aucun bénéfice
ici** : la manette ne possède pas de moteurs à piloter. Au contraire,
déclarer la page PID activerait les pilotes génériques des hôtes (`hid-pid`
sous Linux, pile PID de GameInput sous Windows), qui s'attendent à un
dialogue complet et risqueraient de mal interpréter un descripteur partiel.

Le firmware déclare donc un Output Report « dual-rumble » simple —
`Output (Data, Variable, Absolute)` de 4 octets — exactement le format
qu'écrivent les outils hôtes génériques (hidapi, WebHID) et aligné sur les
champs de l'effet standard `dual-rumble` de la Web Gamepad API.

---

## 2. Structure du paquet HID

### 2.1 Descripteur — extrait ajouté à `HID_REPORT_MAP`

```c
0xC0,             /*   End Collection (Physical)       */

/* Output Report (1) : retour de force « dual-rumble » …              */
0x15, 0x00,       /*   Logical Minimum (0)             */
0x26, 0xFF, 0x00, /*   Logical Maximum (255)           */
0x75, 0x08,       /*   Report Size (8)                 */
0x95, 0x04,       /*   Report Count (4)                */
0x91, 0x02,       /*   Output (Data, Var, Abs)         */
0xC0              /* End Collection (Application)      */
```

L'Output Report partage le **Report ID 0x01** avec l'Input Report — pratique
standard et légale (un clavier partage ainsi son rapport touches et son
rapport LED). Côté GATT, il s'agit de deux caractéristiques distinctes du
service HID, différenciées par leur descripteur Report Reference :

- Input : `{reportId = 0x01, type = 0x01}` — notification ;
- Output : `{reportId = 0x01, type = 0x02}` — écriture (Write et
  Write Without Response, chiffrées après appairage).

### 2.2 Format de l'Output Report n°1 (4 octets)

| Octet | Champ | Type | Plage | Signification |
|:-----:|-------|------|-------|---------------|
| 0 | `strongMagnitude` | u8 | 0..255 | moteur fort (main gauche) — **non nul = ordre de Tare** |
| 1 | `weakMagnitude` | u8 | 0..255 | moteur faible (main droite) — **non nul = ordre de Tare** |
| 2..3 | `duration` | u16 LE | 0..65535 | durée de l'effet (ms) — informative |

Règles de décodage côté ESP32-C6 (`OutputCallbacks::onWrite`) :

- **Toute magnitude non nulle** (`strongMagnitude > 0` **ou**
  `weakMagnitude > 0`) déclenche la tare — le ou les moteurs sollicités
  n'ont pas d'importance ;
- un rapport aux deux magnitudes **nulles** est un ordre d'arrêt de
  vibration : ignoré ;
- **anti-rebond de 250 ms** (`BLE_RUMBLE_DEBOUNCE_MS`, `config.h`) : un
  effet continu relancé périodiquement par l'hôte ne déclenche qu'une seule
  tare par fenêtre (protège la NVS et la trace série) ;
- selon la pile hôte, l'octet de **Report ID `0x01` préfixe** la valeur
  écrite dans la caractéristique GATT (usage de la spécification HID
  Service) — le firmware accepte **les deux formats** : 4 octets nus, ou
  5 octets avec préfixe Report ID.

### 2.3 Exemples de trames valides

```
05 01 FF FF 64 00   → Report ID 1, strong=255, weak=255, durée=100 ms  → TARE
FF 00 64 00         → strong=255, weak=0,   durée=100 ms               → TARE
00 00 00 00         → arrêt de vibration                                → ignoré
```

---

## 3. Exemple Frontend — JavaScript / Web Gamepad API

### 3.1 Détection du gamepad BOB BL AR et déclenchement de l'effet haptique

```html
<!DOCTYPE html>
<html lang="fr">
<head><meta charset="utf-8"><title>BOB BL AR — Tare par rumble</title></head>
<body>
  <p>Appuyez sur la touche <kbd>T</kbd> pour recentrer le point 0 de la tête.</p>
  <p id="status">Recherche de la manette « BOB BL AR »…</p>

<script>
'use strict';

const BOB_NAME = 'BOB BL AR';
let bobGamepad = null;

/* --- Détection du gamepad BOB BL AR --------------------------------- */
window.addEventListener('gamepadconnected', (e) => {
  if (e.gamepad.id.toUpperCase().includes(BOB_NAME)) {
    bobGamepad = e.gamepad;
    document.getElementById('status').textContent =
      'Manette détectée : ' + e.gamepad.id;
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
      if (pad && pad.id.toUpperCase().includes(BOB_NAME)) {
        bobGamepad = pad;
        document.getElementById('status').textContent = 'Manette détectée : ' + pad.id;
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
      'vibrationActuator indisponible — voir la variante WebHID.';
    return;
  }

  /* Un ordre de rumble non nul → l'ESP32-C6 enregistre les angles
     actuels comme nouvelle référence neutre (pitch = roll = yaw = 0). */
  await actuator.playEffect('dual-rumble', {
    duration:        100,    // ms (informative côté firmware)
    strongMagnitude: 1.0,    // moteur fort  → 0..255 sur le fil
    weakMagnitude:   1.0     // moteur faible → 0..255 sur le fil
  });
  document.getElementById('status').textContent = 'Tare envoyée (rumble).';
});
</script>
</body>
</html>
```

> **Conditions de fonctionnement** : `gamepad.vibrationActuator` n'est exposé
> par le navigateur que si la pile de l'OS relaie le rumble vers la manette
> (Chrome/Edge). Une manette BLE HID tierce n'est pas toujours mappée par
> l'hôte — sur macOS en particulier, GCController ne propose l'haptique
> qu'aux manettes MFi. Dans ce cas, utiliser la **variante WebHID** ci-dessous,
> qui écrit directement l'Output Report et fonctionne systématiquement.

### 3.2 Variante robuste — WebHID (Chrome / Edge)

```js
'use strict';

const FILTERS = [{ vendorId: 0x1209, productId: 0xB0B1 }];  // BOB BL AR

async function tareByWebHID(strong = 255, weak = 255, durationMs = 100) {
  /* Nécessite un geste utilisateur (clic) et HTTPS ou localhost. */
  const [device] = await navigator.hid.requestDevice({ filters: FILTERS });
  if (!device) throw new Error('Aucun appareil sélectionné');

  await device.open();
  try {
    /* sendReport(reportId, payload) — le navigateur achemine via la pile
       HID de l'OS : Report ID 1 + {strong, weak, durée LE}. */
    await device.sendReport(1, new Uint8Array([
      strong,
      weak,
      durationMs & 0xFF,          // durée, poids faible
      (durationMs >> 8) & 0xFF    // durée, poids fort
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

VID, PID = 0x1209, 0xB0B1        # BOB BL AR (pid.codes, usage communautaire)
REPORT_ID = 0x01                 # Output Report « dual-rumble »


def find_device():
    """Retourne le chemin hidapi de la manette BOB BL AR."""
    devices = hid.enumerate(VID, PID)
    if not devices:
        raise RuntimeError(
            "BOB BL AR introuvable — appairer la manette au préalable")
    return devices[0]['path']


def tare(path, strong=255, weak=255, duration_ms=100):
    """Envoie l'ordre de rumble : toute magnitude non nulle re-tare la tête.

    hid_write attend le Report ID en premier octet, suivi de la charge
    utile {strongMagnitude, weakMagnitude, durée u16 little-endian}.
    """
    h = hid.device()
    h.open_path(path)
    try:
        buf = [REPORT_ID,
               strong, weak,
               duration_ms & 0xFF, (duration_ms >> 8) & 0xFF]
        if h.write(buf) <= 0:
            raise RuntimeError("Écriture de l'Output Report impossible")
    finally:
        h.close()


if __name__ == '__main__':
    tare(find_device())
    print("Ordre de tare envoyé (rumble HID).")
```

### 4.2 pygame / SDL (mention)

`pygame` expose `Joystick.rumble(low_frequency, high_frequency, duration)` :

```python
import pygame

pygame.init()
pygame.joystick.init()
for i in range(pygame.joystick.get_count()):
    stick = pygame.joystick.Joystick(i)
    if 'BOB BL AR' in stick.get_name().upper():
        stick.init()
        stick.rumble(1.0, 1.0, 100)   # → ordre de Tare côté ESP32-C6
        break
```

> **Réserve** : `pygame.rumble()` traverse la pile de manettes de l'OS
> (SDL), qui ne relaie le rumble que vers les périphériques qu'elle sait
> piloter (manettes du commerce, appareils mappés par un pilote dédié).
> Pour une manette HID tierce, **préférer hidapi** (§ 4.1), qui écrit
> l'Output Report quelle que soit la plateforme.

---

## 5. Comportement détaillé du firmware

### 5.1 Correspondance des fonctions et variables

| Concept du cahier des charges | Implémentation firmware |
|---|---|
| `tare_origin()` | [`HeadTracker::tare()`](../src/head_tracker.cpp) — copie `_rawPitchDeg/_rawRollDeg/_rawYawDeg` dans `_tarePitch/_tareRoll/_tareYaw`, persiste en NVS (clés `tarP`, `tarR`, `tarY`) |
| `offset_pitch/roll/yaw` | `_tarePitch`, `_tareRoll`, `_tareYaw` (soustraits des angles bruts à chaque itération 100 Hz) |
| `current_pitch/roll/yaw` | `_rawPitchDeg`, `_rawRollDeg`, `_rawYawDeg` (volatiles, écrits par la tâche 100 Hz) |
| Callback `onWrite` HID | [`BleGamepadHid::OutputCallbacks::onWrite`](../src/ble_gamepad.cpp) — parse, filtre, anti-rebond, notifie |
| Déclenchement `strongMagnitude > 0` | filtrage dans `onWrite`, politique câblée dans `main.cpp` (`_onRumbleOrder` → `requestTare()`) |
| Recentrage immédiat de l'Input Report | consommation du drapeau `_tareRequested` en tête de `_taskLoop()` : le rapport suivant part des angles recentrés (≤ 10 ms + intervalle de notification `BLE_SEND_MIN_INTERVAL_MS`) |

### 5.2 Choix d'architecture : callback léger + exécution différée

Le callback `onWrite` s'exécute dans la **tâche hôte NimBLE** (pile
limitée). Conformément à la convention du socle — jamais d'appel lourd dans
un callback — il se contente de :

1. décoder la trame (4 octets, avec ou sans préfixe Report ID) ;
2. écarter les ordres nuls et appliquer l'anti-rebond ;
3. tracer sur le port série ;
4. positionner le drapeau `_tareRequested` via le handler enregistré.

La tare complète — copie des offsets et **écriture NVS** — est exécutée par
la **tâche manette 100 Hz** dans son propre contexte. Ce circuit différé
existait déjà pour l'API REST (`POST /api/gamepad/tare` reste fonctionnel) ;
les deux canaux convergent vers la même fonction `tare()`.

### 5.3 Fichiers modifiés

| Fichier | Modification |
|---|---|
| `include/config.h` | constante `BLE_RUMBLE_DEBOUNCE_MS` (250 ms) ; pile tâche manette portée à 8192 o (chemin NVS de la tare) |
| `include/ble_gamepad.h` | `RumbleHandler`, `setRumbleHandler()`, `OutputCallbacks`, membres `_output`/`_rumbleHandler`/`_lastRumbleMs` |
| `src/ble_gamepad.cpp` | Output Report dans `HID_REPORT_MAP`, `onWrite` (parse + anti-rebond + notification), caractéristique créée dans `begin()` |
| `include/head_tracker.h` | `requestTare()`, drapeau `_tareRequested` |
| `src/head_tracker.cpp` | consommation du drapeau en tête de `_taskLoop()` |
| `src/main.cpp` | handler `_onRumbleOrder` + enregistrement après `g_bleGamepad.begin()` |

---

## 6. Procédure de validation

1. Compiler et flasher : `pio run -t upload` ;
2. appairer « BOB BL AR » dans les réglages Bluetooth de l'hôte ;
3. incliner la tête : les axes sont non nuls (testeur de manette de l'OS,
   ou télémétrie WebSocket de l'interface web) ;
4. envoyer un ordre de rumble depuis l'hôte (§ 3 ou § 4) ;
5. contrôler la trace série (115200 bauds) :

```
[BLE] Ordre de rumble : fort=255, faible=255, durée=100 ms
[TRACK] Tare effectué (offsets -12.3, 4.5, 87.6°)
```

6. les axes reviennent à ~0 alors que la tête est restée inclinée ;
7. redémarrer l'ESP32 : les offsets sont conservés (NVS `tarP/tarR/tarY`).

---

## 7. Limites connues

- **`gamepad.vibrationActuator`** n'est exposé que si la pile de l'OS mappe
  le rumble vers la manette ; pour une manette BLE HID tierce, préférer
  WebHID (§ 3.2) ou hidapi (§ 4.1) ;
- **sémantique du canal** : tout rumble émis par un jeu (dégâts, collision…)
  re-tare la position — c'est le comportement voulu (« toute magnitude non
  nulle = ordre de recentrage »), atténué par l'anti-rebond de 250 ms ;
- **aucune vibration physique** : la manette ne comporte pas de moteurs,
  l'Output Report est un canal de commande pur ;
- le canal est **monodirectionnel hôte → manette** et sans accusé de
  réception : la confirmation se lit sur la télémétrie (axes recentrés) ou
  la trace série.
