# BOB BL AR — Documentation système

Manette de jeu **Bluetooth Low Energy (BLE HID)** reconnue nativement par macOS et
Windows, pilotée par les **mouvements de la tête** via un capteur inertiel MPU9250.
L'ESP32-C6 héberge à la fois le firmware temps réel et une **interface web complète**
(point d'accès Wi-Fi autonome + portail captif), accessible depuis un téléphone ou un
ordinateur sans rien installer.

```
        tête ──► MPU9250 ──I2C──► ESP32-C6 ──BLE HID──► macOS / Windows
                                   │  ▲
                                   │  └── réglages, télémétrie 10 Hz
                                   ▼
                        Interface web (AP « BOB BL AR »)
```

---

## Table des matières

1. [Présentation](#1-présentation)
2. [Matériel et câblage](#2-matériel-et-câblage)
3. [Architecture logicielle](#3-architecture-logicielle)
4. [Chaîne de traitement du signal](#4-chaîne-de-traitement-du-signal)
5. [Interface web](#5-interface-web)
6. [Réseau Wi-Fi](#6-réseau-wi-fi)
7. [Manette BLE HID](#7-manette-ble-hid)
8. [Persistance NVS](#8-persistance-nvs)
9. [Référence API REST](#9-référence-api-rest)
10. [Construction et flashage](#10-construction-et-flashage)
11. [Première utilisation](#11-première-utilisation)
12. [Réglage de la manette](#12-réglage-de-la-manette)
13. [Dépannage](#13-dépannage)
14. [Limites connues et pistes d'évolution](#14-limites-connues-et-pistes-dévolution)

---

## 1. Présentation

### 1.1 Principe

| Mouvement de tête | Axe manette | Effet |
|---|---|---|
| Tangage (avant/arrière) | Joystick 1 — **Y** | Y− nez bas (avant), Y+ nez haut |
| Roulis (gauche/droite) | Joystick 1 — **X** | X+ tête penchée à droite |
| Lacet (rotation gauche/droite) | Joystick 2 — **Z** | Z+ tête tournée à droite |
| Coup de tête sec (jerk) | **Bouton 1** | impulsion, avec recharge |

Chaque axe possède ses propres **zone morte**, **sensibilité** et **sens** (inversion).
Un bouton « **tare** » recentre le point 0 : la position actuelle de la tête devient
le neutre. Tous les réglages sont persistés en NVS (survivent aux redémarrages).

### 1.2 Caractéristiques principales

- **Cadence** : lecture capteur + fusion + rapport HID à **100 Hz** (tâche FreeRTOS dédiée)
- **Latence BLE** : rapports envoyés uniquement au changement, débit limité à ~66 Hz
- **Interface web** : SPA par tuiles (dashboard), thème clair/sombre, télémétrie temps
  réel WebSocket, ordre des tuiles personnalisable par glisser-déposer
- **Réseau** : point d'accès autonome **« BOB BL AR »** + connexion optionnelle à une
  box (mode AP+STA simultané) avec portail captif multi-OS
- **Consommation firmware** : ~48 Ko de RAM (15 %), ~1,5 Mo de flash (18 % sur 8 Mo)

### 1.3 Choix techniques structurants

| Sujet | Décision | Raison |
|---|---|---|
| Plateforme | pioarduino 54.03.21 (core Arduino 3.2.1) | la plateforme officielle espressif32 ne supporte pas le C6 en Arduino |
| BLE | NimBLE-Arduino 2.5.1 + profil HID assemblé à la main | la bibliothèque BleGamepad classique repose sur NimBLE 1.x, incompatible C6 |
| Web | ESPAsyncWebServer + AsyncTCP | serveur non bloquant, aucun appel réseau dans `loop()` |
| Fusion | Mahony 6 axes (gyro + accéléro) | léger (~2 Ko), excellent compromis réactivité/dérive pour du jeu |
| Système de fichiers | LittleFS | résistant aux coupures de courant, idéal pour l'interface web |

---

## 2. Matériel et câblage

### 2.1 Liste

| Élément | Référence |
|---|---|
| Carte | ESP32-C6 DevKitC-1 (N8 : 8 Mo flash, RISC-V monocœur) |
| Capteur | MPU9250 (ou clone MPU9255/MPU6500/MPU6050 — détection étendue) |
| Alimentation | USB 5 V (les deux ports conviennent pour l'alim) |
| Fils | Dupont femelle-femelle courts |

### 2.2 Câblage I2C

| MPU9250 | ESP32-C6 | Broche physique DevKitC-1 |
|---|---|---|
| VCC | 3V3 | — |
| GND | GND | — |
| SDA | **GPIO4** | J1 pin 3 |
| SCL | **GPIO5** | J1 pin 4 |

Notes :

- L'adresse I2C (0x68 ou 0x69 selon la broche AD0) est **détectée automatiquement**
  au démarrage — inutile de configurer quoi que ce soit.
- Le bus tourne à **100 kHz** : les tirages internes du C6 (~45 kΩ) et les fils
  Dupont ne supportent pas 400 kHz de façon fiable. Passer à 400 kHz n'a d'intérêt
  qu'avec des résistances de tirage externes de 4,7 kΩ.
- Les broches sont configurables dans `include/config.h` (`PIN_I2C_SDA`,
  `PIN_I2C_SCL`).

### 2.3 Ports USB du DevKitC-1

La carte a **deux** ports USB-C :

| Port | Usage |
|---|---|
| **USB ESP32** (celui du circuit ESP32) | moniteur série de l'application (`Serial.print`), flash |
| UART bridge (circuit WCH) | logs bas niveau du chargeur de démarrage (utile en cas de crash profond) |

Pour suivre les messages du firmware, utilisez le port **USB ESP32**, puis :

```bash
pio device monitor
```

---

## 3. Architecture logicielle

### 3.1 Arborescence

```
BOBBLAR/
├── platformio.ini            # Build : plateforme, bibliothèques, flags
├── partitions.csv            # Découpage flash 8 Mo (app 5 Mo + LittleFS 3 Mo)
├── BobBlAr.md                # ← ce document
├── interface_base.md         # Socle d'origine (spécification architecture web)
├── scripts/
│   └── git_version.py        # Génère include/git_version.h à chaque build
├── include/                  # Headers (les .h contiennent aussi la doc de référence)
│   ├── config.h              # TOUS les réglages constants du projet
│   ├── ahrs_mahony.h         # Filtre de fusion Mahony (header-only)
│   ├── imu_mpu9250.h         # Driver capteur (double adresse + diagnostic bus)
│   ├── head_tracker.h        # Cœur métier : réglages, mapping, tâche 100 Hz
│   ├── ble_gamepad.h         # Profil HID NimBLE (rapport 5 octets)
│   ├── web_server.h          # Serveur web + machines à états Wi-Fi
│   └── git_version.h         # (généré)
├── src/
│   ├── main.cpp              # Séquence d'initialisation
│   ├── imu_mpu9250.cpp       # Lecture capteur, calibrage gyro, scan bus
│   ├── head_tracker.cpp      # Tâche temps réel + NVS + tare
│   ├── ble_gamepad.cpp       # Services HID, advertising, appairage
│   └── web_server.cpp        # Routes REST, WebSocket, portail captif, scan/connexion
└── data/                     # → LittleFS (interface web)
    ├── index.html            # SPA par tuiles
    ├── style.css             # Design system complet (tokens, responsive)
    └── app.js                # Logique front (WS bufferisé, REST, drag & drop)
```

### 3.2 Séquence de démarrage (`main.cpp`)

1. `Serial.begin(115200)` + 1 s d'attente (port USB Serial/JTAG)
2. Montage **LittleFS** (non fatal si absent : le serveur affiche la procédure)
3. `g_headTracker.begin()` — réglages NVS → capteur I2C (adresses 0x68/0x69,
   diagnostic bus) → calibrage gyroscopique (~2 s, **appareil immobile**)
4. `g_webServer.begin()` — code pays Wi-Fi « FR », scan de démarrage (~2 s),
   SoftAP + IP statique, reconnexion STA mémorisée, routes, DNS captif, tâche web
5. `g_bleGamepad.begin()` — NimBLE **après** le Wi-Fi (réservation des buffers
   radio dans cet ordre)
6. `g_headTracker.startTask()` — tâche manette 100 Hz

> Le démarrage complet prend ~5 s (calibrage gyro + scan de démarrage Wi-Fi).

### 3.3 Tâches FreeRTOS (ESP32-C6 monocœur)

| Tâche | Priorité | Pile | Rôle | Période |
|---|---|---|---|---|
| **Gamepad** | 4 | 6144 o | I2C → Mahony → mapping → rapport HID | 10 ms (100 Hz) |
| **WebServer** | 1 | 8192 o | DNS captif, machines à états Wi-Fi, télémétrie WS | 20 ms (50 Hz) |
| **async_tcp** | (lib) | 8192 o | handlers HTTP/WebSocket de la pile asynchrone | événementiel |
| loop() | 1 | — | vide (toute la logique est dans les tâches) | — |

La tâche manette (priorité 4) **preempte** toujours la tâche web : la cadence 100 Hz
est garantie même quand le serveur traite des requêtes.

### 3.4 Règle d'or : les handlers HTTP ne touchent pas au matériel

Les handlers d'ESPAsyncWebServer s'exécutent sur la tâche `async_tcp` (pile limitée).
Ils ne font **jamais** d'appel Wi-Fi (`WiFi.begin`, scan…), NVS (`Preferences`) ou
redémarrage direct : ils posent un flag volatile d'état, attendent (`delay()`), puis
répondent. Les opérations réelles sont orchestrées par la **tâche web** via des
machines à états (`_advanceScan`, `_advanceConnect`, `_restartTaskEntry`).
Ce découpage a été institué après un débordement de pile avéré (crash au scan).

---

## 4. Chaîne de traitement du signal

### 4.1 Vue d'ensemble (tâche Gamepad, 100 Hz)

```
MPU9250 ──I2C 100 kHz──► accél (g) + gyro (°/s)
        │
        ▼
   Mahony 6 axes (quaternion)  ──►  pitch / roll / yaw (°)
        │                              │
        │  getGravityG()               ▼
        ▼                        angles tarés (− offsets tare)
  accélération linéaire                │
  = mesure − pesanteur                 ▼
        │                       zones mortes + sensibilité/axe
        ▼                       → J1 X/Y, J2 Z ∈ [−127, +127]
  filtre EMA (α = 0,7)                 │
        │                              ▼
        ▼                        rapport HID 5 octets ──BLE──► hôte
  jerk (seuil + hystérésis
  + recharge) ──► Bouton 1
```

### 4.2 Fusion Mahony (`ahrs_mahony.h`)

- Comparaison pesanteur estimée (quaternion) ↔ mesurée (accéléromètre) ;
  l'erreur (produit vectoriel) corrige le gyroscope en proportionnel + intégral.
- Gains : 2×Kp = 2,0 (convergence), 2×Ki = 0,1 (compensation du biais résiduel).
- **Le lacet n'est pas contraint** par l'accéléromètre : c'est une intégration
  gyroscopique, dérive lente (~1°/min). Le bouton « tare » corrige le point 0.
- Conventions : pitch > 0 nez haut · roll > 0 à droite · yaw > 0 tourné à droite.

### 4.3 Détection du jerk (Bouton 1)

1. `linéaire = (mesure − pesanteur_estimée) × 9,81` (m/s², dans le repère capteur)
2. Norme filtrée par EMA α = 0,7
3. Déclenchement si `filtre > seuil` (défaut 15 m/s²) et recharge écoulée (500 ms)
4. Relâchement avec hystérésis (retour sous 60 % du seuil)

Le seuil et la recharge sont réglables dans l'interface ; un marqueur rouge sur la
barre d'accélération de la télémétrie visualise le seuil en direct.

### 4.4 Mapping angle → axe

Pour chaque axe, avec zone morte `dz` et pleine déviation `fd` **propres à l'axe** :

```
|angle| ≤ dz            → 0            (anti-jitter)
|angle| > dz            → ±127 × (|angle| − dz) / (fd − dz), borné à ±127
```

- Valeur **petite** = axe réactif (peu d'angle pour la pleine course)
- Valeur **grande** = axe précis (grande amplitude de tête nécessaire)
- Les trois axes sont réglables **indépendamment** (depuis la version à
  sensibilités par axe) : typiquement Roulis 25° / Lacet 45° pour compenser
  l'amplitude naturelle plus faible du roulis.

---

## 5. Interface web

### 5.1 Accès

| Situation | Accès |
|---|---|
| AP autonome (par défaut) | Wi-Fi « BOB BL AR », mot de passe `bobblar2026`, puis http://192.168.4.1/ — **le portail s'ouvre automatiquement** à la connexion |
| Connecté à une box (mode STA) | `http://<IP_attribuée>/` (l'IP est affichée au moniteur série à la connexion) |

### 5.2 Organisation

- **Dashboard par tuiles** (tuile Manette, tuile Wi-Fi, tuile Système) — ordre
  personnalisable par glisser-déposer (appui long 500 ms, persisté en localStorage)
- **Section Manette** : angles en direct, joysticks visualisés (pastilles),
  barre d'accélération + seuil jerk, LED Bouton 1, formulaires de réglages + tare
- **Section Wi-Fi** : scan des réseaux, connexion avec mot de passe (persisté)
- **Section Système** : version, uptime, heap, état BLE/IMU, redémarrage
- **Thème** clair/sombre persisté ; responsive (points de rupture 768/480 px)

### 5.3 Télémétrie WebSocket

- Endpoint `/ws`, diffusion **10 Hz** par la tâche web
- Côté navigateur : messages **bufferisés**, rendu regroupé via
  `requestAnimationFrame` → zéro jitter d'affichage, charge CPU minimale
- Format : `{"type":"tele","up":…,"heap":…,"yaw":…,"pitch":…,"roll":…,"lin":…,
  "j1x":…,"j1y":…,"j2z":…,"btn":…,"ble":…,"imu":…,"i2e":…}`
- Reconnexion automatique après 2 s en cas de coupure

---

## 6. Réseau Wi-Fi

### 6.1 Mode AP+STA simultané

- **SoftAP** : « BOB BL AR », canal 6, IP fixe 192.168.4.1, jusqu'à 4 clients.
  Toujours actif — c'est le canal de secours/paramétrage, même quand la manette
  est connectée à une box.
- **STA** : connexion optionnelle à un réseau existant (identifiants mémorisés en
  NVS, reconnexion automatique au démarrage). Utile pour joindre l'interface sans
  quitter son réseau habituel.

### 6.2 Portail captif

Le serveur DNS (port 53) résout **tous les noms** vers 192.168.4.1, et les URLs de
sonde des OS (Apple `/hotspot-detect.html`, Android `/generate_204`, Windows
`/connecttest.txt` + `/ncsi.txt`, Firefox `/canonical.html` + `/success.txt`,
Kindle…) répondent par une redirection vers l'interface → l'ouverture automatique
de la page à la connexion au réseau « BOB BL AR ».

> L'ouverture automatique ne se déclenche qu'au moment de la connexion Wi-Fi.
> Si le téléphone était déjà connecté : basculer le Wi-Fi off/on ou « Oublier ce
> réseau » puis se reconnecter.

### 6.3 Scan Wi-Fi — pourquoi c'est du sûr

Le scan est la partie la plus délicate du système (trois pièges rencontrés et
corrigés, voir §13) :

1. **Code pays « FR »** appliqué au pilote dès le démarrage — sans lui, le pilote
   en mode « monde » ne scanne que les canaux 1-11 et **rate les box françaises
   sur canal 12/13**.
2. **Scan de démarrage** en mode STA pur (avant l'activation de l'AP) : scan
   « premier plan », le plus fiable. Son résultat est servi instantanément au
   premier clic « Scanner » ; il sert aussi de diagnostic radio.
3. **Machine à états** (demande → scan → résultat/échec) exécutée par la tâche
   web : suspend temporairement la reconnexion STA (le pilote refuse un scan
   pendant une connexion), dwell 300 ms/canal, réessais toutes les 150 ms dans un
   budget de 8 s, puis restaure l'état. Le handler HTTP, lui, ne fait qu'attendre
   et répondre.

La connexion à un réseau suit la même logique : handler → machine à états
(`_advanceConnect`) → NVS + `WiFi.begin()` dans la tâche web, timeout 15 s.
**Jamais** de `WiFi.disconnect()` en mode AP+STA : cela tuerait le point d'accès.

---

## 7. Manette BLE HID

### 7.1 Profil

| Élément | Valeur |
|---|---|
| Nom Bluetooth | « BOB BL AR » |
| Apparence GAP | 0x03C4 (HID Gamepad → icône manette côté hôte) |
| VID/PID | 0x1209 / 0xB0B1 (vendor-assigned, pid.codes) |
| Appairage | **Just Works** + bonding + Secure Connections (accepté macOS/Windows) |
| Services | HID (0x1812), Battery (0x180F, annoncé 100 %), Device Information |
| Rapport d'entrée | **5 octets**, notification GATT, Report Reference n°1 |

### 7.2 Rapport HID

| Octet | Champ | Contenu |
|---|---|---|
| 0 | `x1` | Joystick 1 X — roulis (int8, −127..+127) |
| 1 | `y1` | Joystick 1 Y — tangage |
| 2 | `z2` | Joystick 2 Z — lacet |
| 3 | `rz2` | Joystick 2 Rz — réservé (0) |
| 4 | `buttons` | bit0 = Bouton 1 (jerk), bit1 = libre |

Le rapport n'est émis **qu'au changement** (comparaison octet à octet) et au plus
toutes les 15 ms → économie de radio et de pile hôte.

### 7.3 Appairage

Réglages Bluetooth de macOS/Windows → « Ajouter un appareil » → **BOB BL AR**.
La manette apparaît comme une manette de jeu générique. Le bonding est mémorisé :
la reconnexion est automatique quand la carte s'allume.

---

## 8. Persistance NVS

Deux espaces (`Preferences`) :

| Namespace | Clés | Contenu |
|---|---|---|
| `gamepad` | `dzP` `dzR` `dzY` | zones mortes pitch/roll/yaw (°) |
| | `fdP` `fdR` `fdY` | pleines déviations par axe (°) |
| | `jerk` `cool` | seuil jerk (m/s²), recharge (ms) |
| | `inv` | bitmap inversions X/Y/Z |
| | `tarP` `tarR` `tarY` | offsets de tare (°) |
| `wifi` | `ssid` `pass` | identifiants du réseau mémorisé |

Chaque écriture est **vérifiée par relecture** (convention du socle) — l'API REST
retourne une erreur si la NVS n'a pas confirmé. Les réglages sont bornés à deux
niveaux : validation 400 côté API REST, puis re-bornage systématique dans
`applySettings()` (défense en profondeur).

> Migration : l'ancienne clé unique `full` (sensibilité globale) amorce les trois
> axes `fdP/fdR/fdY` au premier démarrage, puis est purgée.

---

## 9. Référence API REST

Toutes les routes répondent en JSON. Les POST acceptent
`application/x-www-form-urlencoded`.

| Méthode | Route | Paramètres | Réponse |
|---|---|---|---|
| GET | `/api/system/status` | — | version, uptime, heap, état AP/STA, BLE, IMU |
| POST | `/api/system/restart` | — | redémarrage différé de 3 s |
| GET | `/api/wifi/scan` | — | `{networks:[{ssid,rssi,secure}…]}` — 503 si scan déjà en cours |
| POST | `/api/wifi/connect` | `ssid`, `pass` | `{status:"connected",ip}` ou `{status:"failed"}` |
| GET | `/api/gamepad/config` | — | tous les réglages courants |
| POST | `/api/gamepad/config` | `dzPitch` `dzRoll` `dzYaw` `fullDeflectPitch` `fullDeflectRoll` `fullDeflectYaw` `jerkThresh` `jerkCooldown` `invX` `invY` `invZ` (champ absent = valeur conservée) | réglages normalisés, ou 400 avec message |
| POST | `/api/gamepad/tare` | — | recentre le point 0 |

Bornes de validation : zones mortes 0-45° · sensibilités 10-90° · seuil jerk
3-60 m/s² · recharge 100-3000 ms.

---

## 10. Construction et flashage

### 10.1 Commandes

```bash
pio run                 # compilation
pio run -t upload       # flash du firmware
pio run -t uploadfs     # flash de l'interface web (data/ → LittleFS)
pio device monitor      # moniteur série 115200 (port USB ESP32)
pio run -t clean        # si le build se corrompt (fichiers .d manquants)
```

Après modification de `data/` (interface), **les deux** uploads sont nécessaires :
`upload` pour le firmware, `uploadfs` pour les fichiers web. Puis recharger la
page de force dans le navigateur (Cmd+Shift+R) pour contourner le cache.

### 10.2 Configuration de build (`platformio.ini`)

| Flag | Rôle |
|---|---|
| plateforme **pioarduino 54.03.21** | seule à embarquer le core Arduino 3.x avec le support C6 |
| `ARDUINO_USB_MODE=1` + `ARDUINO_USB_CDC_ON_BOOT=1` | route `Serial` vers l'USB Serial/JTAG matériel du C6 — **les deux sont obligatoires** |
| `CORE_DEBUG_LEVEL=1` | logs d'erreur du core uniquement |
| `CONFIG_ASYNC_TCP_STACK_SIZE=8192` | pile de la tâche async_tcp (4096 par défaut : trop juste) |
| `CONFIG_ASYNC_TCP_QUEUE_SIZE=64` | file d'événements réseau |

Bibliothèques : ESPAsyncWebServer ^3.11.0, AsyncTCP ^3.4.10, ArduinoJson ^7.0.0,
NimBLE-Arduino ^2.5.1.

### 10.3 Partitionnement flash 8 Mo

| Partition | Offset | Taille | Contenu |
|---|---|---|---|
| nvs | 0x9000 | 20 Ko | réglages + identifiants Wi-Fi |
| otadata | 0xE000 | 8 Ko | données OTA |
| app0 (factory) | 0x10000 | 5 Mo | firmware (~1,5 Mo utilisés) |
| spiffs | 0x510000 | ~3 Mo | LittleFS : interface web |

Le numéro de version affiché dans l'interface provient de
`scripts/git_version.py`, exécuté à chaque build (état git + date).

---

## 11. Première utilisation

1. Câbler le MPU9250 (§2.2), alimenter par USB (port ESP32 de préférence)
2. Attendre ~5 s : la bannière de démarrage défile au moniteur série, dont :
   - `[TRACK] Capteur détecté : …` (le MPU doit être détecté)
   - `[WEB] Scan de démarrage : N réseau(x)` (la radio doit voir des réseaux)
   - `[WEB] Point d'accès démarré : BOB BL AR`
3. Sur le téléphone/PC : se connecter au Wi-Fi **« BOB BL AR »** (`bobblar2026`)
   — le portail s'ouvre tout seul (sinon ouvrir http://192.168.4.1/)
4. Sur l'hôte de jeu : Bluetooth → ajouter l'appareil **« BOB BL AR »**
5. Optionnel : section Wi-Fi de l'interface → scanner → se connecter à une box
6. Poser la tête droite → **« Recalibrer (tare) »** → jouer

---

## 12. Réglage de la manette

| Effet souhaité | Réglage |
|---|---|
| Manette plus nerveuse / précise | **baisser** la sensibilité de l'axe (ex. 20°) |
| Gestes amples, progression douce | **augmenter** la sensibilité (ex. 50°) |
| Le curseur tremble à l'arrêt | **augmenter** la zone morte de l'axe |
| Axe dans le mauvais sens | cocher l'inversion correspondante |
| Bouton 1 se déclenche tout seul | **augmenter** le seuil jerk (ex. 20-25 m/s²) |
| Bouton 1 difficile à déclencher | **baisser** le seuil (ex. 10 m/s²) |
| Bouton 1 en rafale | augmenter la recharge (ex. 800 ms) |
| Le neutre a dérivé | **« Recalibrer (tare) »** tête droite |

Observer la barre d'accélération en direct : elle visualise le filtre et le seuil
du jerk — le geste doit faire franchir nettement le marqueur rouge.

---

## 13. Dépannage

Tous les points suivants ont été rencontrés réellement sur ce projet.

| Symptôme | Cause | Solution |
|---|---|---|
| `MPU9250 indisponible` au démarrage | câblage SDA/SCL inversés, mauvaise broche, ou tirages manquants | vérifier §2.2 ; le log `[IMU] Diagnostic bus I2C` tranche : 0 périphérique = câblage ; > 8 adresses = bus bloqué (court-circuit SDA/SCL) |
| Aucun log de l'application au moniteur | branché sur le port UART bridge | utiliser le **port USB ESP32** (§2.3) |
| Scan Wi-Fi : 0 réseau | canaux 12/13 invisibles (code pays « monde ») | corrigé : code pays « FR » natif ; si régresse, vérifier `esp_wifi_set_country_code` dans `begin()` |
| Scan : « pilote Wi-Fi occupé » | STA en pleine reconnexion (le pilote refuse) | corrigé : la machine à états suspend la reconnexion le temps du scan |
| Crash/redémarrage au scan | appel Wi-Fi/NVS depuis un handler async_tcp (pile trop petite) | corrigé : orchestration par la tâche web + pile 8192 — ne pas régresser |
| Erreur de compilation `USBSerial was not declared` | flag `ARDUINO_USB_MODE=1` absent | vérifier `platformio.ini` §10.2 |
| `Partitions overlap` au build | partitions.csv modifié | revenir au schéma §10.3 |
| `opening dependency file .d` | build interrompu qui a corrompu `.pio/build` | `pio run -t clean` puis recompiler |
| Upload impossible (port occupé) | moniteur série ouvert | fermer le moniteur, relancer l'upload |
| Portail captif qui ne s'ouvre pas | téléphone déjà connecté à l'AP | Wi-Fi off/on ou oublier le réseau puis se reconnecter |
| Lacet qui dérive lentement | dérive gyroscopique normale (Mahony 6 axes) | « Recalibrer (tare) » — ou évoluer vers Mahony 9 axes (§14) |

---

## 14. Limites connues et pistes d'évolution

- **Lacet en intégration libre** (Mahony 6 axes) : dérive ~1°/min. Piste : activer
  le magnétomètre du MPU9250 (déjà lu, non fusionné) pour un Mahony 9 axes — au
  prix d'un calibrage magnétique supplémentaire.
- **Niveau de batterie fixe à 100 %** : aucun capteur de tension sur le DevKit.
  Piste : pont diviseur + ADC, ou modélisation par tension d'alimentation.
- **Bouton 2 libre** (bit 1 du rapport) : disponible dans le rapport HID, non
  affecté. Piste : double coup de tête, détection de « hochement » n°/oui.
- **Diffusion multi-hôtes non garantie** : la notification GATT part à tous les
  clients abonnés, mais le bonding et le comportement de reconnexion sont pensés
  pour **un seul hôte actif** (le premier appairé) — suffisant pour l'usage prévu.
- **Pas d'OTA à ce jour** : la partition otadata est réservée mais aucune logique
  de mise à jour en vol n'est branchée. Piste : implémenter `Update` + route
  d'upload binaire dans l'interface web.

---

*Document généré pour le projet BOB BL AR — ESP32-C6 · PlatformIO · FreeRTOS ·
NimBLE-Arduino · ESPAsyncWebServer · ArduinoJson · LittleFS.*
