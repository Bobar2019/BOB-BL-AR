# INTERFACE_BASE — Socle commun ESP32-S3 + Interface Web

> **Document de référence / template de démarrage.**
> Architecture standardisée issue des projets BOB-CONTROL (ROV) et BOBCNC, éprouvée en production.
> Objectif : démarrer un nouveau projet ESP32-S3 instantanément avec la même base — connectivité,
> portail captif, interface web par tuiles, design system et backend asynchrone.

**Version du socle :** 1.0.0 — Septembre 2026
**Cible :** ESP32-S3 (DevKitC-1) · PlatformIO · Arduino Core · FreeRTOS · LittleFS

---

## Sommaire

1. [Vue d'ensemble & arborescence](#1-vue-densemble--arborescence)
2. [Connectivité & Réseau](#2-connectivité--réseau)
3. [Interface Web (Frontend)](#3-interface-web-frontend)
4. [Design System personnalisé](#4-design-system-personnalisé)
5. [Backend ESP32](#5-backend-esp32)
6. [Checklist de démarrage d'un nouveau projet](#6-checklist-de-démarrage-dun-nouveau-projet)
7. [Conventions transverses & commandes](#7-conventions-transverses--commandes)

---

## 1. Vue d'ensemble & arborescence

### 1.1 Principes directeurs

| Principe | Traduction concrète |
|---|---|
| **Zéro framework frontend** | HTML/CSS/JS natifs servis depuis LittleFS — aucune chaîne de build, aucun CDN obligatoire |
| **Tout asynchrone côté serveur** | ESPAsyncWebServer + AsyncTCP : aucun handler ne bloque la boucle réseau |
| **Double cœur exploité** | Core 0 = Wi-Fi/Web/DNS · Core 1 = logique temps réel du projet |
| **Persistance NVS systématique** | Toute configuration utilisateur survit au redémarrage (Preferences, écriture vérifiée par relecture) |
| **Offline-first** | L'interface fonctionne sans Internet ; les polices Google Fonts sont la seule tolérance réseau |
| **Mobile-first** | Conçu pour téléphone/tablette en Wi-Fi direct sur l'appareil, expériences tactiles iOS/Android |

### 1.2 Arborescence de référence

```
mon_projet/
├── platformio.ini              # Build PlatformIO (board, flags, lib_deps)
├── partitions.csv              # Table de partitions personnalisée (optionnel)
├── scripts/
│   └── git_version.py          # Injection de la version Git au build
├── include/
│   ├── config.h                # TOUTES les constantes matérielles & réseau (sections numérotées)
│   ├── git_version.h           # Généré au build (ne pas éditer)
│   ├── web_server.h            # Interface du serveur web (classe + singleton g_webServer)
│   └── ...modules_projet.h     # Un header par sous-système métier
├── src/
│   ├── main.cpp                # setup() : init séquentielle + lancement des tâches FreeRTOS
│   ├── web_server.cpp          # AP/STA, portail captif, DNS, routes REST, WebSocket
│   └── ...modules_projet.cpp   # Implémentations métier
├── data/                       # → LittleFS (servi par le serveur web)
│   ├── index.html              # Shell SPA : topbar + dashboard tuiles + sections
│   ├── style.css               # Design system complet (tokens + composants + responsive)
│   ├── app.js                  # Navigation SPA, WebSocket, REST, édition des tuiles
│   ├── favicon.svg / images    # Assets statiques
│   └── vendor/                 # Bibliothèques JS vendorisées (ex. three.min.js)
└── test/                       # Tests host-side éventuels
```

---

## 2. Connectivité & Réseau

### 2.1 Cahier des charges

- **Mode AP+STA simultané** : l'ESP32 diffuse son propre point d'accès (SSID/mot de passe projet)
  ET tente de rejoindre le dernier réseau client mémorisé — les deux coexistent sans se détruire.
- **Portail captif universel** : à la connexion au Wi-Fi de l'appareil, l'OS du client
  (Android, iOS/macOS, Windows, Firefox, Kindle) **ouvre automatiquement** l'interface web.
- **DNS captif** : toutes les résolutions DNS sont interceptées et résolues vers l'IP de l'AP.
- **Gestion STA non bloquante** : scan des réseaux, connexion, sauvegarde NVS des identifiants,
  statut temps réel remonté à l'interface — sans jamais tuer le SoftAP ni geler le serveur HTTP.

### 2.2 Constantes (config.h — section « Réseau »)

```cpp
/* =========================================================================
 * SECTION N : CONFIGURATION RÉSEAU WI-FI ET POINT D'ACCÈS
 * ========================================================================= */

/** @brief SSID du point d'accès Wi-Fi autonome */
constexpr const char* AP_SSID           = "MON_PROJET_AP";

/** @brief Mot de passe du point d'accès (min. 8 caractères WPA2) */
constexpr const char* AP_PASSWORD       = "motdepasse2026";

/** @brief Adresse IP statique du point d'accès */
constexpr const char* AP_IP             = "192.168.4.1";

/** @brief Canal Wi-Fi du point d'accès */
constexpr uint8_t  AP_CHANNEL           = 6;

/** @brief Nombre maximum de clients Wi-Fi simultanés */
constexpr uint8_t  AP_MAX_CLIENTS       = 4;

/** @brief Stack et priorité de la tâche Web (Core 0) */
constexpr uint32_t STACK_SIZE_WEB       = 8192;
constexpr uint8_t  PRIORITY_WEB         = 1;
```

### 2.3 Démarrage AP + reconnexion STA (dans `WebServer::begin()`)

```cpp
void MonWebServer::begin() {
    /* 1. Point d'accès avec IP statique (le serveur répond toujours à 192.168.4.1) */
    WiFi.mode(WIFI_AP_STA);                      /* AP et STA simultanés */
    WiFi.softAPConfig(
        IPAddress(192, 168, 4, 1),               /* IP locale   */
        IPAddress(192, 168, 4, 1),               /* Gateway     */
        IPAddress(255, 255, 255, 0)              /* Masque      */
    );
    WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CLIENTS);

    /* 2. Événements Wi-Fi asynchrones → flags volatils (statut STA) */
    WiFi.onEvent(onWiFiEvent);

    Serial.println("[WEB] Point d'accès démarré : " + String(AP_SSID));
    Serial.println("[WEB] IP : " + WiFi.softAPIP().toString());

    /* 3. Reconnexion au dernier réseau mémorisé (NVS, non bloquante) */
    Preferences prefs;
    prefs.begin("wifi", true);                   /* lecture seule */
    String savedSSID = prefs.getString("ssid", "");
    String savedPass = prefs.getString("pass", "");
    prefs.end();

    if (savedSSID.length() > 0) {
        Serial.println("[WEB] Tentative de connexion à : " + savedSSID);
        WiFi.begin(savedSSID.c_str(), savedPass.c_str());
        /* Non bloquant : les événements Wi-Fi mettront à jour le statut */
    }

    /* 4. Routes HTTP, WebSocket, DNS captif, démarrage serveur */
    _setupRoutes();
    _setupWebSocket();
    _setupCaptiveDNS();
    _server.begin();
    Serial.println("[WEB] Serveur HTTP démarré sur le port 80");

    /* 5. Tâche FreeRTOS sur Core 0 (traite le DNS + broadcasts périodiques) */
    xTaskCreatePinnedToCore(_taskEntry, "WebServer", STACK_SIZE_WEB,
                            this, PRIORITY_WEB, nullptr, 0 /* CORE_WIFI */);
}
```

**Callback d'événements** (statut STA consulté par les handlers et la télémétrie) :

```cpp
volatile bool _wifi_sta_got_ip  = false;   /* IP obtenue sur le réseau client */
volatile bool _wifi_sta_failed  = false;   /* échec (mauvais mot de passe…)   */

static void onWiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            _wifi_sta_got_ip = true;
            _wifi_sta_failed = false;
            Serial.println("[WEB] STA connecté — IP : " + WiFi.localIP().toString());
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            _wifi_sta_got_ip = false;
            _wifi_sta_failed = true;
            Serial.println("[WEB] STA déconnecté");
            break;
        default:
            break;
    }
}
```

### 2.4 DNS captif

```cpp
#include <DNSServer.h>
DNSServer _dns;

void MonWebServer::_setupCaptiveDNS() {
    /* Toute requête DNS (n'importe quel domaine) → IP de l'AP */
    _dns.start(53, "*", IPAddress(192, 168, 4, 1));
    Serial.println("[WEB] DNS captif démarré (port 53, redirection → 192.168.4.1)");
}

/* À appeler régulièrement depuis la boucle de la tâche Web (Core 0) */
void MonWebServer::processDNS() {
    _dns.processNextRequest();
}
```

### 2.5 Portail captif — détection automatique multi-OS

Chaque OS sonde des URLs dédiées au branchement sur un Wi-Fi. En répondant par une
**redirection 302 vers notre IP**, l'OS conclut « pas d'Internet → portail captif » et
**ouvre automatiquement** la page de connexion.

> **Ordre d'enregistrement critique** : ces routes doivent être enregistrées
> **AVANT** `serveStatic()` et le handler `onNotFound`.

```cpp
void MonWebServer::_setupRoutes() {

    /* ===================== PORTAIL CAPTIF (détection OS) =====================
     * L'OS du client interroge ces URLs pour tester l'accès Internet ;
     * un 302 vers notre IP déclenche l'ouverture automatique du portail. */

    /* Android / ChromeOS */
    _server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->redirect("http://192.168.4.1/");
    });
    _server.on("/gen_204", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->redirect("http://192.168.4.1/");
    });
    _server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->redirect("http://192.168.4.1/");
    });

    /* iOS / macOS (Apple) */
    _server.on("/library/test/success.html", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->redirect("http://192.168.4.1/");
    });

    /* Windows (Microsoft NCSI) */
    _server.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->redirect("http://192.168.4.1/");
    });
    _server.on("/redirect", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->redirect("http://192.168.4.1/");
    });

    /* Firefox */
    _server.on("/canonical.html", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->redirect("http://192.168.4.1/");
    });

    /* Kindle / Fire OS */
    _server.on("/kindle-wifi/wifistub.html", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->redirect("http://192.168.4.1/");
    });

    /* ... routes REST métier (§ 5.4) ... */

    /* Fichiers statiques : la racine sert index.html depuis LittleFS */
    _server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    /* ============ CATCHALL — redirection des requêtes « captivées » ============
     * Toute requête dont le Host n'est pas 192.168.4.1 a été détournée par le
     * DNS captif (ex. www.google.com → 192.168.4.1) → redirect vers l'interface.
     * Host correct + LittleFS vide → page de secours (instructions d'upload). */
    _server.onNotFound([](AsyncWebServerRequest* request) {
        String host = request->getHeader("Host") ? request->getHeader("Host")->value() : "";
        if (host != "192.168.4.1" && host != "192.168.4.1:80") {
            request->redirect("http://192.168.4.1/");
            return;
        }
        String path = request->url();
        if (path == "/" || path == "/index.html" || path == "/index.htm") {
            request->send(200, "text/html",
                "<!DOCTYPE html><html lang='fr'><head><meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>MON_PROJET</title><style>"
                "body{background:#131417;color:#e0e0e0;font-family:monospace;"
                "display:flex;align-items:center;justify-content:center;height:100vh;margin:0}"
                "</style></head><body><div style='text-align:center'>"
                "<h1>LittleFS vide</h1>"
                "<p>Ex&eacute;cutez&nbsp;: pio run -t uploadfs</p>"
                "</div></body></html>");
            return;
        }
        request->send(404, "text/plain", "Not Found");
    });
}
```

### 2.6 Scan des réseaux (GET /api/wifi/scan)

```cpp
void MonWebServer::_handleWifiScan(AsyncWebServerRequest* request) {
    int n = WiFi.scanNetworks(true);            /* scan asynchrone */

    /* Attente de la fin du scan (max 5 s) */
    unsigned long start = millis();
    while (WiFi.scanComplete() == WIFI_SCAN_RUNNING && (millis() - start) < 5000) {
        delay(10);
    }

    n = WiFi.scanComplete();
    if (n < 0) n = 0;

    JsonDocument doc;
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
        JsonObject net = arr.add<JsonObject>();
        net["ssid"]   = WiFi.SSID(i);
        net["rssi"]   = WiFi.RSSI(i);
        net["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
    WiFi.scanDelete();                          /* libère la mémoire du scan */
}
```

### 2.7 Connexion client + sauvegarde NVS (POST /api/wifi/connect)

```cpp
/**
 * Corps form-urlencoded : ssid=...&pass=...
 *
 * RÈGLE D'OR : ne JAMAIS appeler WiFi.disconnect() en mode AP_STA — cela
 * désactive le SoftAP et coupe le client web en pleine requête.
 * WiFi.begin() avec de nouveaux identifiants remplace proprement la
 * connexion STA précédente sans affecter le point d'accès.
 */
void MonWebServer::_handleWifiConnect(AsyncWebServerRequest* request) {
    if (!request->hasParam("ssid", true)) {
        request->send(400, "application/json", "{\"error\":\"Paramètre 'ssid' manquant\"}");
        return;
    }

    String ssid = request->getParam("ssid", true)->value();
    String pass = request->hasParam("pass", true)
                  ? request->getParam("pass", true)->value() : "";

    /* Sauvegarde NVS : identifiants mémorisés pour le prochain démarrage */
    Preferences prefs;
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();

    _wifi_sta_got_ip = false;
    _wifi_sta_failed = false;
    WiFi.begin(ssid.c_str(), pass.c_str());

    /* Attente NON bloquante (max 15 s) : delay() cède le CPU aux tâches
     * Wi-Fi/HTTP pour que le serveur continue de répondre pendant l'attente. */
    unsigned long start = millis();
    while (!_wifi_sta_got_ip && !_wifi_sta_failed && (millis() - start) < 15000) {
        delay(100);
    }

    if (_wifi_sta_got_ip && WiFi.status() == WL_CONNECTED) {
        String ip = WiFi.localIP().toString();
        request->send(200, "application/json",
                      "{\"status\":\"connected\",\"ip\":\"" + ip + "\"}");
    } else {
        request->send(200, "application/json",
                      "{\"status\":\"failed\",\"message\":\"Vérifiez le mot de passe\"}");
    }
}
```

**Côté frontend**, la page Wi-Fi consomme ces deux routes : un `fetch('/api/wifi/scan')`
remplit la liste (SSID + barre RSSI + cadenas), la sélection ouvre le champ mot de passe,
et le bouton Connecter poste sur `/api/wifi/connect` puis affiche l'IP obtenue.

---

## 3. Interface Web (Frontend)

### 3.1 Cahier des charges

- **SPA sans routeur** : une seule page `index.html` ; les « pages » sont des `<section>`
  masquées/affichées en JS (`display:none/block`) — navigation instantanée, zéro rechargement.
- **Point d'entrée = dashboard par tuiles** : grille 2 colonnes (max 600 px), chaque tuile est
  un lien `<a>` avec icône SVG + libellé, qui bascule vers sa section.
- **Tuiles réorganisables** (expérience écran d'accueil iPhone) : mode édition par bouton ou
  appui long 500 ms, tremblement jiggle, glisser-déposer Pointer Events, ordre persisté en
  `localStorage`.
- **Bouton retour** flottant sur chaque section pour revenir au dashboard.
- **Pied de page** : version Git du firmware (servie par `/api/system/status`), auteur, licence.
- **Anti-FOUC** : le thème mémorisé est appliqué avant le premier rendu (script inline).

### 3.2 Shell HTML (`data/index.html`)

```html
<!DOCTYPE html>
<html lang="fr">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, viewport-fit=cover">
    <meta name="theme-color" content="#131417">
    <meta name="apple-mobile-web-app-capable" content="yes">
    <meta name="mobile-web-app-capable" content="yes">
    <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
    <meta name="apple-mobile-web-app-title" content="MON_PROJET">
    <title>MON_PROJET — Tableau de Bord</title>
    <link rel="icon" type="image/svg+xml" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><rect width='100' height='100' rx='20' fill='%23131417'/><text y='72' x='50' font-family='Arial' font-size='40' font-weight='bold' fill='%2300d9ff' text-anchor='middle'>MP</text></svg>">
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
    <link href="https://fonts.googleapis.com/css2?family=Michroma&family=Source+Code+Pro:wght@400;500;600;700&display=swap" rel="stylesheet">
    <script>
        /* Thème sombre par défaut ; préférence mémorisée appliquée AVANT le
           premier rendu pour éviter tout flash de thème au chargement. */
        (function () {
            var t = 'dark';
            try { t = localStorage.getItem('monprojet_theme') || 'dark'; } catch (_) {}
            document.documentElement.setAttribute('data-theme', t === 'light' ? 'light' : 'dark');
        })();
    </script>
    <link rel="stylesheet" href="style.css">
</head>
<body>

    <!-- Barre de statut fixe : logo + badges live + bascule de thème -->
    <header id="topbar">
        <div class="logo">MON_PROJET</div>
        <div class="topbar-right">
            <div class="status-badges">
                <span id="badge-ws" class="badge badge-off">WS: Déconnecté</span>
                <!-- ...autres badges projet (mode, watchdog, capteurs...) -->
            </div>
            <button id="theme-toggle" class="theme-toggle" type="button"
                    aria-label="Passer au thème clair" title="Passer au thème clair">
                <!-- SVG soleil (mode sombre) + SVG lune (mode clair) -->
            </button>
        </div>
    </header>

    <!-- ===== VUE DASHBOARD (tuiles) ===== -->
    <main id="dashboard" class="index-container">
        <header class="index-header">
            <h1>TABLEAU DE BORD</h1>
            <p>Sélectionnez une fonction pour commencer</p>
        </header>

        <!-- Barre d'édition drag & drop des tuiles -->
        <div class="dash-edit-bar">
            <button id="btnEditDash" class="dash-edit-btn" type="button">
                <span>Réorganiser</span>
            </button>
            <button id="btnSaveDash" class="dash-edit-btn dash-edit-save" type="button" style="display:none">
                <span>Terminer</span>
            </button>
        </div>

        <!-- Grille de tuiles : UNE tuile = UNE section SPA -->
        <nav class="nav-grid" id="nav-grid">
            <a class="nav-tile" data-tile-id="wifi" href="#section-wifi">
                <svg width="40" height="40" viewBox="0 0 24 24" fill="none"
                     stroke="currentColor" stroke-width="2"><!-- icône Wi-Fi --></svg>
                <span>Wi-Fi</span>
            </a>
            <a class="nav-tile" data-tile-id="settings" href="#section-settings">
                <svg width="40" height="40" viewBox="0 0 24 24" fill="none"
                     stroke="currentColor" stroke-width="2"><!-- icône réglages --></svg>
                <span>Paramètres</span>
            </a>
            <!-- ...ajouter une tuile par fonctionnalité... -->
        </nav>
    </main>

    <!-- ===== SECTIONS SPA (masquées par défaut, affichées par app.js) ===== -->
    <section id="section-wifi" class="section-panel page-container" style="display:none">
        <h2>Connexion Wi-Fi</h2>
        <div class="card">
            <!-- liste des réseaux + champ mot de passe + bouton Connecter -->
        </div>
    </section>

    <section id="section-settings" class="section-panel page-container" style="display:none">
        <h2>Paramètres</h2>
        <!-- accordéons <details class="card acc"> par groupe de réglages -->
    </section>

    <!-- Bouton retour flottant (visible hors dashboard) -->
    <a id="btn-back" class="back-top-btn" href="#" aria-label="Retour au tableau de bord"></a>

    <!-- Pied de page : version Git dynamique + mentions -->
    <footer class="app-footer">
        <span>MON_PROJET <span id="app-version">v1.0.0</span></span>
        <span aria-hidden="true"> | </span>
        <span>Auteur : Prénom Nom</span>
        <span aria-hidden="true"> | </span>
        <span>Licence MIT</span>
    </footer>

    <script src="app.js"></script>
</body>
</html>
```

### 3.3 Navigation SPA (`data/app.js`)

```js
/* ===== NAVIGATION SPA ===== */
const dashboard = document.getElementById('dashboard');
const btnBack   = document.getElementById('btn-back');
const sections  = Array.from(document.querySelectorAll('.section-panel'));

function showSection(id) {
    dashboard.style.display = 'none';
    sections.forEach(s => s.style.display = 'none');
    const target = document.getElementById(id);
    if (target) target.style.display = 'block';
    if (btnBack) btnBack.style.display = 'flex';
    window.scrollTo(0, 0);
}

function showDashboard() {
    sections.forEach(s => s.style.display = 'none');
    dashboard.style.display = 'flex';
    if (btnBack) btnBack.style.display = 'none';
    window.scrollTo(0, 0);
}

/* Clic sur une tuile → section correspondante (href="#section-xxx") */
document.getElementById('nav-grid').addEventListener('click', (e) => {
    const tile = e.target.closest('.nav-tile');
    if (tile) showSection(tile.getAttribute('href').substring(1));
});

/* Bouton retour + touche Escape */
btnBack.addEventListener('click', (e) => { e.preventDefault(); showDashboard(); });
document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') showDashboard();
});
```

### 3.4 Ordre des tuiles persisté (localStorage)

```js
/* L'ordre survit aux rechargements : chaque tuile porte un data-tile-id STABLE
   (contrairement à nth-child, dont l'indice changerait à chaque réordonnancement). */
function saveDashboardOrder(grid) {
    const order = Array.from(grid.querySelectorAll('.nav-tile')).map(t => t.dataset.tileId);
    try { localStorage.setItem('monprojet_dashOrder', JSON.stringify(order)); } catch (_) {}
}

function applyOrder(grid, order) {
    if (!Array.isArray(order)) return;
    grid.classList.add('no-enter-anim');
    const byId = {};
    grid.querySelectorAll('.nav-tile').forEach(t => { byId[t.dataset.tileId] = t; });
    order.forEach(id => { if (byId[id]) grid.appendChild(byId[id]); });
    setTimeout(() => grid.classList.remove('no-enter-anim'), 50);
}

/* Au chargement */
(function restoreOrder() {
    const grid = document.getElementById('nav-grid');
    try {
        const cached = localStorage.getItem('monprojet_dashOrder');
        if (cached) applyOrder(grid, JSON.parse(cached));
    } catch (_) {}
})();
```

### 3.5 Client WebSocket (télémétrie temps réel)

```js
/* Connexion au flux temps réel — reconnexion automatique après coupure */
let ws = null;

function connectWS() {
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    ws = new WebSocket(proto + '://' + location.host + '/ws');

    ws.onopen = () => setBadge('badge-ws', 'WS: Connecté', 'badge-ok');
    ws.onclose = () => {
        setBadge('badge-ws', 'WS: Déconnecté', 'badge-off');
        setTimeout(connectWS, 2000);           /* reconnexion auto */
    };
    ws.onmessage = (ev) => renderTelemetry(JSON.parse(ev.data));
}
```

**Règle de performance** (indispensable au-delà de ~10 Hz) : bufferiser les messages entrants
et ne rafraîchir le DOM **qu'une fois par `requestAnimationFrame`** — jamais dans le handler
`onmessage` lui-même. Mémoïser les `getElementById` et vérifier qu'une valeur a changé avant
d'écrire dans le DOM.

### 3.6 Appels REST (lecture/écriture de configuration)

```js
/* Pattern standard d'un formulaire de réglages :
   1. GET au chargement → peupler les champs
   2. POST form-urlencoded à la sauvegarde
   3. réponse {status:"ok"} ou {error:"..."} */

async function loadConfig() {
    const res = await fetch('/api/xxx/config');
    const cfg = await res.json();
    /* ...peupler les champs du formulaire... */
}

async function saveConfig() {
    const res = await fetch('/api/xxx/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'param1=' + encodeURIComponent(v1) + '&param2=' + encodeURIComponent(v2)
    });
    const data = await res.json();
    if (data.status === 'ok') alert('Configuration enregistrée (NVS).');
    else alert('Erreur : ' + (data.error || 'inconnue'));
}
```

---

## 4. Design System personnalisé

### 4.1 Cahier des charges

- **Tokens centralisés** : toutes les couleurs, rayons et polices vivent dans `:root` —
  changer le thème = changer un bloc, jamais une valeur dispersée.
- **Thème sombre par défaut, thème clair opt-in** : surcharge sous `html[data-theme="light"]`,
  bascule persistée en localStorage, appliquée avant le premier rendu (anti-FOUC).
- **Palette signature** : fond bleu-noir profond, cartes gris-bleu, accents cyan/magenta/violet,
  sémantique vert/rouge/orange.
- **Polices** : Michroma (titres, identité) + Source Code Pro (corps, valeurs).
- **Micro-interactions** : hover lift sur cartes/tuiles, tremblement iOS en mode édition,
  transitions douces `cubic-bezier(0.4, 0, 0.2, 1)`.
- **Responsive** : 2 points de rupture uniquement — 768 px et 480 px.

### 4.2 Tokens (`data/style.css`)

```css
/* ===== RESET ===== */
*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

/* ===== VARIABLES (thème sombre par défaut) ===== */
:root {
    /* Surfaces */
    --bg-primary:   #131417;
    --bg-secondary: #1e1f26;
    --bg-card:      #252830;
    --bg-input:     #1a1b21;

    /* Textes */
    --text-primary:   #ffffff;
    --text-secondary: #aaaebc;
    --text-dim:       #666b7a;

    /* Accents signature */
    --accent-cyan:    #00d9ff;
    --accent-magenta: #ff3e9d;
    --accent-purple:  #b084ff;

    /* Sémantique */
    --success: #00e676;
    --danger:  #ff3b30;
    --warning: #ffb300;
    --info:    #29b6f6;

    /* Bordures & surfaces translucides (inversées en thème clair) */
    --border-color: #444857;
    --border-subtle: #3d4150;
    --divider:     rgba(255, 255, 255, 0.05);
    --panel-faint: rgba(255, 255, 255, 0.03);
    --track-bg:    rgba(255, 255, 255, 0.08);

    /* Géométrie */
    --radius: 12px;
    --radius-sm: 8px;

    /* Typographie */
    --font-body:  'Source Code Pro', monospace;
    --font-title: 'Michroma', 'Source Code Pro', sans-serif;

    color-scheme: dark;   /* contrôles natifs en sombre */
}

/* ===== THÈME CLAIR (opt-in, surcharge des tokens uniquement) ===== */
html[data-theme="light"] {
    color-scheme: light;

    --bg-primary:   #eef0f4;
    --bg-secondary: #ffffff;
    --bg-card:      #ffffff;
    --bg-input:     #f4f5f8;

    --text-primary:   #1a1c22;
    --text-secondary: #4a4f5e;
    --text-dim:       #6e7382;

    --accent-cyan:    #007a99;    /* assombri pour le contraste sur blanc */
    --accent-magenta: #d0207c;
    --accent-purple:  #6f42c1;

    --success: #0e8c50;
    --danger:  #d32f2f;
    --warning: #b26a00;

    --border-color:  #c9ced9;
    --border-subtle: #d5d9e2;
    --divider:     rgba(0, 0, 0, 0.08);
    --panel-faint: rgba(0, 0, 0, 0.03);
    --track-bg:    rgba(0, 0, 0, 0.08);
}

body {
    background: var(--bg-primary);
    color: var(--text-primary);
    font-family: var(--font-body);
    min-height: 100vh;
}
```

### 4.3 Dashboard par tuiles

```css
/* ===== DASHBOARD ===== */
.index-container {
    display: flex;
    flex-direction: column;
    align-items: center;
    padding: 40px 16px 80px;
}
.index-header { text-align: center; margin-bottom: 30px; }
.index-header h1 {
    font-family: var(--font-title);
    font-size: 1.8rem;
    letter-spacing: 4px;
    color: var(--text-primary);
}

/* Grille : 2 colonnes fixes, largeur maximale contenue */
.nav-grid {
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    gap: 20px;
    width: 100%;
    max-width: 600px;
}

/* Tuile = carte cliquable : icône + libellé empilés */
.nav-tile {
    background: var(--bg-card);
    border: 1px solid var(--border-color);
    border-radius: var(--radius);
    padding: 35px 20px;
    text-align: center;
    cursor: pointer;
    transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);
    text-decoration: none;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    min-height: 140px;
    position: relative;
    overflow: hidden;
    color: var(--text-secondary);
    /* Neutralise les gestes natifs — requis pour l'appui long iOS */
    -webkit-touch-callout: none;
    -webkit-user-select: none;
    user-select: none;
    -webkit-user-drag: none;
}
.nav-tile:hover {
    border-color: var(--accent-cyan);
    transform: translateY(-4px);
    box-shadow: 0 8px 24px rgba(0, 217, 255, 0.15);
    color: var(--text-primary);
}
.nav-tile svg { margin-bottom: 12px; }
.nav-tile span {
    font-family: var(--font-body);
    font-size: 0.8rem;
    letter-spacing: 2px;
    text-transform: uppercase;
}
```

### 4.4 Animation de tremblement (mode édition, effet écran d'accueil iPhone)

```css
/* En édition : toutes les tuiles tremblotent, curseur « grab » */
.nav-grid.editing .nav-tile {
    cursor: grab;
    animation: tileJiggle 0.35s ease-in-out infinite;
    touch-action: none;               /* le drag tactile ne scrolle pas la page */
}
.nav-grid.editing .nav-tile:active { cursor: grabbing; }

/* Désynchronisation : chaque tuile garde SA phase propre (data-tile-id
   stable au réordonnancement — nth-child ferait « sauter » l'animation).
   Durées et délais négatifs légèrement différents → tremblement organique. */
.nav-grid.editing .nav-tile[data-tile-id="wifi"]     { animation-duration: 0.32s; animation-delay: -0.05s; }
.nav-grid.editing .nav-tile[data-tile-id="settings"] { animation-duration: 0.38s; animation-delay: -0.13s; }
/* ...une règle par data-tile-id... */

/* Poignée de drag affichée en édition */
.nav-grid.editing .nav-tile::after {
    content: "✥";
    position: absolute;
    top: 8px;
    right: 10px;
    font-size: 0.9rem;
    color: var(--text-dim);
}

/* Tremblement : légère rotation ±1,5° en boucle continue */
@keyframes tileJiggle {
    0%, 100% { transform: rotate(-1.5deg); }
    50%      { transform: rotate(1.5deg); }
}

/* Appui long en cours : grossissement léger — feedback de soulèvement imminent */
.nav-tile.pressing { transform: scale(1.06); transition: transform 0.15s ease; }

/* Tuile en cours de drag : le « trou » dans la grille reste stable */
.nav-tile.dragging-ghost {
    opacity: 0.3;
    filter: grayscale(40%);
    animation: none !important;
    transform: none !important;
}
```

### 4.5 Composants de base

```css
/* ===== CARTES ===== */
.card {
    background: var(--bg-card);
    border: 1px solid var(--border-color);
    border-radius: var(--radius);
    padding: 20px;
    margin-bottom: 20px;
    position: relative;
    overflow: hidden;
}
.card::before {              /* liseré d'accent en haut de carte */
    content: "";
    position: absolute;
    top: 0; left: 0; right: 0;
    height: 2px;
    background: linear-gradient(90deg, var(--accent-cyan), var(--accent-magenta));
}

/* ===== BOUTONS ===== */
.btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    padding: 10px 22px;
    border: none;
    border-radius: var(--radius-sm);
    font-family: var(--font-body);
    font-size: 0.85rem;
    letter-spacing: 1px;
    cursor: pointer;
    transition: all 0.2s;
    text-decoration: none;
}
.btn-primary   { background: linear-gradient(135deg, #00b4d8, #00d9ff); color: #000; }
.btn-success   { background: linear-gradient(135deg, #00c853, #00e676); color: #000; }
.btn-danger    { background: linear-gradient(135deg, #d32f2f, #ff3b30); color: #fff; }
.btn:hover     { transform: translateY(-1px); box-shadow: 0 4px 14px rgba(0, 217, 255, 0.25); }
.btn:active    { transform: translateY(0); }
.btn:disabled  { opacity: 0.5; cursor: not-allowed; transform: none; }

/* ===== BADGES DE STATUT (topbar) ===== */
.badge {
    padding: 4px 10px;
    border-radius: 20px;
    font-size: 0.68rem;
    letter-spacing: 1px;
    font-weight: 600;
    white-space: nowrap;
}
.badge-ok    { background: rgba(0, 230, 118, 0.15); color: var(--success); border: 1px solid var(--success); }
.badge-off   { background: var(--panel-faint); color: var(--text-dim); border: 1px solid var(--border-subtle); }
.badge-real  { background: rgba(0, 217, 255, 0.12); color: var(--accent-cyan); border: 1px solid var(--accent-cyan); }

/* ===== TOPBAR FIXE ===== */
#topbar {
    position: sticky;
    top: 0;
    z-index: 100;
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 12px 20px;
    background: rgba(19, 20, 23, 0.85);
    backdrop-filter: blur(10px);
    border-bottom: 1px solid var(--divider);
}
#topbar .logo {
    font-family: var(--font-title);
    font-size: 1rem;
    letter-spacing: 2px;
    color: var(--accent-cyan);
}

/* ===== FORMULAIRES ===== */
.settings-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(250px, 1fr));
    gap: 14px;
    margin-bottom: 20px;
}
.settings-grid label {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 10px;
    font-size: 0.85rem;
    color: var(--text-secondary);
    gap: 10px;
}
.settings-grid input, .settings-grid select {
    width: 160px;
    padding: 8px 10px;
    background: var(--bg-input);
    border: 1px solid var(--border-color);
    border-radius: var(--radius-sm);
    color: var(--text-primary);
    font-family: var(--font-body);
    outline: none;
}
.settings-grid input:focus, .settings-grid select:focus { border-color: var(--accent-cyan); }
```

### 4.6 Responsive

```css
/* Deux points de rupture, pas plus */
@media (max-width: 768px) {
    .nav-grid { grid-template-columns: repeat(2, 1fr); gap: 14px; }
    .nav-tile { min-height: 110px; padding: 25px 12px; }
    .nav-tile svg { width: 34px; height: 34px; }
    .nav-tile span { font-size: 0.7rem; letter-spacing: 1px; }
    .index-header h1 { font-size: 1.4rem; letter-spacing: 2px; }
    .page-container { padding: 20px 12px 80px; }
    .settings-grid { grid-template-columns: 1fr; }
}

@media (max-width: 480px) {
    /* les grilles internes passent en colonne unique */
}
```

---

## 5. Backend ESP32

### 5.1 Cahier des charges

- **Bibliothèques asynchrones** exclusivement : `ESPAsyncWebServer` (fork mathieucarbou,
  compatible core 3.x) + `AsyncTCP` + `ArduinoJson` v7 — aucun appel bloquant dans un handler.
- **Un module = un couple header/cpp + un singleton global** (`g_webServer`, `g_sensors`…) :
  initialisation séquentielle dans `setup()`, aucun cycle d'initialisation croisée.
- **Tâche FreeRTOS dédiée** sur Core 0 : traite le DNS captif, diffuse la télémétrie
  WebSocket, exécute les travaux différés (redémarrage, OTA…).
- **API REST uniforme** : GET pour lire, POST form-urlencoded pour écrire, réponses JSON
  `{status:"ok"}` / `{error:"..."}` avec codes HTTP 400/500.
- **NVS** : namespaces par domaine (`wifi`, `config`, un par module), écriture **vérifiée
  par relecture** (sentinelle) avant de confirmer le succès.

### 5.2 platformio.ini de référence

```ini
[env:esp32-s3-devkitc-1]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200

board_build.partitions = partitions.csv        ; optionnel : partition table custom
board_build.filesystem = littlefs
build_flags = 
    -D ARDUINO_USB_MODE=1                      ; USB-CDC natif au boot
    -D ARDUINO_USB_CDC_ON_BOOT=1

; Version Git injectée dans include/git_version.h (git describe --tags)
extra_scripts = pre:scripts/git_version.py

lib_deps = 
    mathieucarbou/ESPAsyncWebServer @ ^3.6.0   ; fork maintenu, core 3.x OK
    mathieucarbou/AsyncTCP @ ^3.3.2
    bblanchon/ArduinoJson @ ^7.0.0
    ; ...drivers matériel du projet (PCA9685, BNO08x, NeoPixel, INA226...)
```

> **Note core 3.x** : le framework Arduino-ESP32 3.x a supprimé les anciennes API canal
> (ledcSetup/ledcAttachPin) au profit du pilotage **par broche** (ledcAttach/ledcWriteTone/
> ledcWrite/ledcDetach) — vérifier la version du core avant tout snippet PWM.

### 5.3 Squelette web_server.h

```cpp
#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "config.h"

class MonWebServer {
public:
    MonWebServer();

    /** @brief AP + STA + routes + DNS + serveur + tâche FreeRTOS (Core 0) */
    void begin();

    /** @brief À appeler par la tâche Web : traite les requêtes DNS captives */
    void processDNS();

    /** @brief Diffuse un message JSON à tous les clients WebSocket */
    void broadcast(const String& json);

    /** @brief IP du point d'accès (affichage diagnostics) */
    String getAPIP() const { return WiFi.softAPIP().toString(); }

private:
    AsyncWebServer    _server;      /* port 80            */
    AsyncWebSocket    _ws;          /* endpoint /ws       */
    DNSServer         _dns;         /* port 53, wildcard  */

    /* --- Séquence d'initialisation --- */
    void _setupRoutes();
    void _setupCaptiveDNS();
    void _setupWebSocket();

    /* --- Handlers REST --- */
    void _handleWifiScan(AsyncWebServerRequest* request);
    void _handleWifiConnect(AsyncWebServerRequest* request);
    void _handleSystemStatus(AsyncWebServerRequest* request);
    /* ...un _handleXxx par ressource métier... */

    /* --- WebSocket --- */
    void _onWSEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                    AwsEventType type, void* arg, uint8_t* data, size_t len);

    /* --- Tâche FreeRTOS --- */
    static void _taskEntry(void* param);
    void _taskLoop();               /* DNS + broadcast périodique */
};

/* Instance globale (définie dans web_server.cpp, appelée depuis main.cpp) */
extern MonWebServer g_webServer;

#endif /* WEB_SERVER_H */
```

### 5.4 Pattern d'un handler REST (lecture/écriture NVS)

```cpp
/* Handler GET : sérialise la config courante en JSON */
void MonWebServer::_handleXxxConfigGet(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["param1"] = ...;
    doc["param2"] = ...;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

/* Handler POST : lit le form-data, borne, écrit NVS vérifiée, répond JSON.
   Convention : champ absent = valeur courante conservée. */
void MonWebServer::_handleXxxConfigPost(AsyncWebServerRequest* request) {
    int v1 = request->hasParam("param1", true)
             ? request->getParam("param1", true)->value().toInt() : _valeurCourante1;

    /* 1. Validation → 400 immédiat si invalide */
    if (v1 < PARAM1_MIN || v1 > PARAM1_MAX) {
        request->send(400, "application/json", "{\"error\":\"Paramètre 1 invalide (min-max)\"}");
        return;
    }

    /* 2. Écriture NVS vérifiée par relecture (convention du socle) */
    Preferences prefs;
    if (!prefs.begin("monmodule", false)) {
        request->send(500, "application/json", "{\"error\":\"Ecriture NVS impossible\"}");
        return;
    }
    prefs.putInt("param1", v1);
    const bool ok = (prefs.getInt("param1", INT_MIN) == v1);   /* relecture */
    prefs.end();
    if (!ok) {
        request->send(500, "application/json", "{\"error\":\"Ecriture NVS non confirmée\"}");
        return;
    }

    /* 3. Application à chaud + réponse + log préfixé */
    _valeurCourante1 = v1;
    Serial.println("[WEB] Config monmodule : param1=" + String(v1));
    request->send(200, "application/json", "{\"status\":\"ok\",\"param1\":" + String(v1) + "}");
}
```

### 5.5 WebSocket (télémétrie + commandes)

```cpp
void MonWebServer::_setupWebSocket() {
    _ws.onEvent([this](AsyncWebSocket* server, AsyncWebSocketClient* client,
                       AwsEventType type, void* arg, uint8_t* data, size_t len) {
        _onWSEvent(server, client, type, arg, data, len);
    });
    _server.addHandler(&_ws);
    Serial.println("[WEB] WebSocket initialisé sur /ws");
}

void MonWebServer::_onWSEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                               AwsEventType type, void* arg, uint8_t* data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            Serial.println("[WS] Client connecté : " + String(client->id()));
            break;
        case WS_EVT_DISCONNECT:
            Serial.println("[WS] Client déconnecté : " + String(client->id()));
            break;
        case WS_EVT_MESSAGE: {
            /* Commande entrante : {"cmd":"xxx","value":...} → dispatcher métier */
            break;
        }
        default:
            break;
    }
}

/* Diffusion périodique — appelée depuis _taskLoop (jamais depuis un handler HTTP) */
void MonWebServer::broadcast(const String& json) {
    if (_ws.count() > 0) _ws.textAll(json);
}
```

### 5.6 Tâche FreeRTOS du serveur (Core 0)

```cpp
void MonWebServer::_taskEntry(void* param) {
    static_cast<MonWebServer*>(param)->_taskLoop();
}

void MonWebServer::_taskLoop() {
    TickType_t lastWake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(TELEMETRY_PERIOD_MS);   /* ex. 50 ms = 20 Hz */

    for (;;) {
        /* 1. DNS captif : répondre aux résolutions des clients AP */
        _dns.processNextRequest();

        /* 2. Nettoyage des clients WebSocket morts */
        _ws.cleanupClients();

        /* 3. Télémétrie périodique : construire le JSON et diffuser */
        String json = buildTelemetryJson();
        broadcast(json);

        vTaskDelayUntil(&lastWake, period);   /* période stricte, pas de dérive */
    }
}
```

### 5.7 main.cpp — séquence d'amorçage type

```cpp
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "config.h"
#include "web_server.h"
/* ...modules métier... */

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n==============================================");
    Serial.println("  MON_PROJET " GIT_VERSION " - Firmware");
    Serial.println("  ESP32-S3 PlatformIO / FreeRTOS");
    Serial.println("==============================================\n");

    /* 1. NVS + LittleFS (un échec de montage est loggé, non fatal) */
    Preferences prefs;
    prefs.begin("config", true);
    /* ...charger la config persistée... */
    prefs.end();

    if (!LittleFS.begin(true)) {
        Serial.println("[MAIN] ERREUR : échec de montage LittleFS !");
    }

    /* 2. Modules métier (I2C, capteurs, actionneurs...) — ordre déterministe */

    /* 3. Serveur web : AP + STA + portail captif + routes + tâche Core 0 */
    g_webServer.begin();

    /* 4. Tâches temps réel sur Core 1 (contrôle, capteurs...) */
    xTaskCreatePinnedToCore(vTaskControle, "Controle", 4096,
                            nullptr, 5, nullptr, 1);
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));   /* vide : tout vit dans les tâches FreeRTOS */
}
```

---

## 6. Checklist de démarrage d'un nouveau projet

1. **Créer l'arborescence** du § 1.2 (copier `scripts/git_version.py` et le squelette
   `web_server.h/.cpp` d'un projet existant, renommer la classe et le singleton).
2. **platformio.ini** : copier le modèle du § 5.2, ajuster `board_build.partitions`
   (ou le retirer pour la table par défaut) et `lib_deps` (ne garder que le socle web,
   ajouter les drivers métier).
3. **config.h** : définir `AP_SSID` / `AP_PASSWORD` (mot de passe ≥ 8 caractères),
   l'IP de l'AP, les constantes réseau et les sections métier.
4. **main.cpp** : bannière + séquence du § 5.7 ; valider la chaîne
   `pio run` → `pio run -t upload` → `pio run -t uploadfs`.
5. **Frontend** : partir du shell du § 3.2, remplacer logo/favicon/titre, créer une
   tuile + une section par fonctionnalité ; vérifier `node --check data/app.js`.
6. **Premier test réseau** : se connecter au SSID de l'AP depuis un téléphone →
   le portail captif doit s'ouvrir automatiquement ; tester `/api/wifi/scan` et la
   connexion STA.
7. **Brancher le métier** : handlers REST (§ 5.4), télémétrie WebSocket (§ 5.5),
   tâches temps réel Core 1.
8. **Versionner** : `git init`, premier commit, puis `git tag vX.0.0` — la version
   affichée dans le footer suit `git describe` au build.

---

## 7. Conventions transverses & commandes

### 7.1 Conventions de code

| Convention | Règle |
|---|---|
| **Logs série** | `Serial.println("[TAG] message")` — un tag par module (`[MAIN]`, `[WEB]`, `[WS]`, `[SENSORS]`…), messages en français, `ERREUR :` en préfixe d'erreur |
| **NVS** | Namespace par domaine ; toute écriture est **relue et comparée** avant de répondre `ok` |
| **Handlers HTTP** | Jamais de calcul long : validation → NVS → réponse ; les travaux lourds passent par une tâche FreeRTOS différée (ex. redémarrage planifié) |
| **Actions diffères** | Redémarrage post-sauvegarde via tâche détachée 3 s (`ESP.restart()`), jamais dans le handler |
| **JSON** | ArduinoJson v7 (`JsonDocument` + `to<JsonArray>()`) ; réponses REST toujours `application/json` |
| **Frontend** | Aucune dépendance build ; vendoriser les libs JS tierces dans `data/vendor/` ; styles uniquement via tokens CSS |
| **Version** | Injectée par `scripts/git_version.py` (git describe) — jamais codée en dur ailleurs que le fallback HTML |
| **Destruction de tâche** | Toute tâche éphémère se termine par `vTaskDelete(NULL)` et neutralise son flag d'anti-réentrance |

### 7.2 Commandes PlatformIO

```bash
pio run                    # Compiler le firmware
pio run -t upload          # Téléverser le firmware (USB)
pio run -t uploadfs        # Téléverser data/ vers LittleFS (obligatoire après
                           # toute modification HTML/CSS/JS)
pio run -t buildfs         # Construire l'image LittleFS sans la flasher
pio device monitor -b 115200   # Moniteur série de diagnostic
```

### 7.3 Pièges connus du socle (retours d'expérience)

- **Portail captif silencieux** : les routes de détection OS doivent être enregistrées
  *avant* `serveStatic()` et `onNotFound`, sinon elles sont avalées par le catchall.
- **Perte du SoftAP à la connexion client** : symptomatique d'un `WiFi.disconnect()`
  appelé avant `WiFi.begin()` — à proscrire en mode AP_STA.
- **Interface cassée après modification web** : firmware flashé mais LittleFS obsolète
  (ou l'inverse) — toujours flasher les deux (`upload` + `uploadfs`) quand `data/` change.
- **Page blanche au premier flash** : LittleFS vide → le handler catchall doit servir la
  page de secours avec la commande à exécuter.
- **Télémétrie saccadée** : rendu DOM exécuté dans `onmessage` au lieu d'être bufferisé
  et rafraîchi par `requestAnimationFrame`.
- **Version périmée dans le footer** : la version est figée au build — recompiler et
  reflasher **après** avoir posé le tag Git pour afficher la bonne version.
- **Appui long iOS qui ouvre le menu système** : les propriétés
  `-webkit-touch-callout: none` + `user-select: none` sur les tuiles sont obligatoires.
