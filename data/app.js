'use strict';

/* ============================================================
   BOB BL AR — Application SPA
   Socle : interface_base.md §3
   - Navigation SPA sans routeur (sections display:none/block)
   - Ordre des tuiles persisté (localStorage, data-tile-id stable)
   - Mode édition : appui long 500 ms + jiggle + drag Pointer Events
   - WebSocket /ws : bufferisation + rendu 1× par requestAnimationFrame
   - REST : GET pour lire, POST form-urlencoded pour écrire
   ============================================================ */

/* ===== CONSTANTES ===== */
const JOY_RANGE = 127;        /* pleine échelle HID int8            */
const JOY_TRAVEL_PX = 40;     /* déplacement max du point joystick  */
const LIN_MAX_MS2 = 60;       /* pleine échelle de la barre d'acc.  */
const WS_RECONNECT_MS = 2000; /* reconnexion WebSocket              */
const SYS_POLL_MS = 5000;     /* rafraîchissement /api/system/status */
const LONGPRESS_MS = 500;     /* appui long → mode édition tuiles   */
const STATUS_TIMEOUT_MS = 6000;

/* ===== RACCOURCIS DOM ===== */
const $id = (id) => document.getElementById(id);

const dashboard = $id('dashboard');
const navGrid = $id('nav-grid');
const btnBack = $id('btn-back');
const sections = Array.from(document.querySelectorAll('.section-panel'));

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

/* ===== ÉCRITURES DOM MÉMOÏSÉES ===== */
/* À 10 Hz on ne réécrit dans le DOM que si la valeur a changé. */
const prevDom = {};
function setText(el, text) {
    if (!el || prevDom[el.id] === text) return;
    prevDom[el.id] = text;
    el.textContent = text;
}
function setStyle(el, prop, val) {
    if (!el) return;
    const key = el.id + '.' + prop;
    if (prevDom[key] === val) return;
    prevDom[key] = val;
    el.style[prop] = val;
}

/* ===== BADGES (topbar) ===== */
function setBadge(id, text, cls) {
    const el = $id(id);
    if (!el) return;
    const className = 'badge ' + cls;
    if (prevDom['b:' + id] === text + '|' + className) return;
    prevDom['b:' + id] = text + '|' + className;
    el.textContent = text;
    el.className = className;
}

/* ===== ZONE DE STATUT (formulaires) ===== */
function setStatus(id, msg, cls) {
    const el = $id(id);
    if (!el) return;
    el.textContent = msg;
    el.className = 'form-status' + (cls ? ' ' + cls : '');
    clearTimeout(el._timer);
    if (msg) {
        el._timer = setTimeout(() => {
            el.textContent = '';
            el.className = 'form-status';
        }, STATUS_TIMEOUT_MS);
    }
}

/* ============================================================
   NAVIGATION SPA
   ============================================================ */
let activeSection = null;

function showSection(id) {
    sections.forEach(s => { s.style.display = 'none'; });
    dashboard.style.display = 'none';
    const target = $id(id);
    if (target) target.style.display = 'block';
    activeSection = id;
    btnBack.style.display = 'flex';
    window.scrollTo(0, 0);
    /* chargement paresseux des données de la section affichée */
    if (id === 'section-gamepad') { loadGamepadConfig(); loadBleName(); }
    if (id === 'section-wifi' && !wifiScannedOnce) scanWifi();
    if (id === 'section-system') startSystemPolling();
    else stopSystemPolling();
}

function showDashboard() {
    sections.forEach(s => { s.style.display = 'none'; });
    dashboard.style.display = 'flex';
    activeSection = null;
    btnBack.style.display = 'none';
    stopSystemPolling();
    window.scrollTo(0, 0);
}

/* Clic sur une tuile → section correspondante (href="#section-xxx").
   Absorbé en mode édition ou juste après un drag. */
let suppressNextClick = false;
navGrid.addEventListener('click', (e) => {
    const tile = e.target.closest('.nav-tile');
    if (!tile) return;
    e.preventDefault();
    if (navGrid.classList.contains('editing')) return;
    if (suppressNextClick) { suppressNextClick = false; return; }
    showSection(tile.getAttribute('href').substring(1));
});

btnBack.addEventListener('click', (e) => { e.preventDefault(); showDashboard(); });
document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && activeSection) showDashboard();
});

/* ============================================================
   THÈME (sombre par défaut, bascule persistée)
   ============================================================ */
const themeToggle = $id('theme-toggle');

function refreshThemeButton() {
    const light = document.documentElement.getAttribute('data-theme') === 'light';
    const label = light ? 'Passer au thème sombre' : 'Passer au thème clair';
    themeToggle.setAttribute('aria-label', label);
    themeToggle.title = label;
}
themeToggle.addEventListener('click', () => {
    const light = document.documentElement.getAttribute('data-theme') === 'light';
    const next = light ? 'dark' : 'light';
    document.documentElement.setAttribute('data-theme', next);
    try { localStorage.setItem('bobblar_theme', next); } catch (_) {}
    refreshThemeButton();
});
refreshThemeButton();

/* ============================================================
   ORDRE DES TUILES (localStorage, data-tile-id stable)
   ============================================================ */
function saveDashboardOrder(grid) {
    const order = Array.from(grid.querySelectorAll('.nav-tile')).map(t => t.dataset.tileId);
    try { localStorage.setItem('bobblar_dashOrder', JSON.stringify(order)); } catch (_) {}
}

function applyOrder(grid, order) {
    if (!Array.isArray(order)) return;
    grid.classList.add('no-enter-anim');
    const byId = {};
    grid.querySelectorAll('.nav-tile').forEach(t => { byId[t.dataset.tileId] = t; });
    order.forEach(id => { if (byId[id]) grid.appendChild(byId[id]); });
    setTimeout(() => grid.classList.remove('no-enter-anim'), 50);
}

(function restoreOrder() {
    try {
        const cached = localStorage.getItem('bobblar_dashOrder');
        if (cached) applyOrder(navGrid, JSON.parse(cached));
    } catch (_) {}
})();

/* ============================================================
   MODE ÉDITION + DRAG & DROP (Pointer Events)
   - entrée : bouton « Réorganiser » OU appui long 500 ms sur une tuile
   - drag : la tuile suivie devient fantôme, le « trou » reste stable,
     la tuile survolée détermine le point d'insertion
   - sortie : bouton « Terminer » (sauvegarde de l'ordre)
   ============================================================ */
const btnEditDash = $id('btnEditDash');
const btnSaveDash = $id('btnSaveDash');

function setEditMode(on) {
    navGrid.classList.toggle('editing', on);
    btnEditDash.style.display = on ? 'none' : 'inline-flex';
    btnSaveDash.style.display = on ? 'inline-flex' : 'none';
}
btnEditDash.addEventListener('click', () => setEditMode(true));
btnSaveDash.addEventListener('click', () => {
    setEditMode(false);
    saveDashboardOrder(navGrid);
});

let dragState = null;   /* {tile, pointerId, moved} */
let longPress = null;   /* {tile, timer, startX, startY} */

navGrid.addEventListener('pointerdown', (e) => {
    if (e.button !== undefined && e.button !== 0 && e.pointerType === 'mouse') return;
    const tile = e.target.closest('.nav-tile');
    if (!tile) return;

    if (navGrid.classList.contains('editing')) {
        /* --- début du drag --- */
        e.preventDefault();
        dragState = { tile, pointerId: e.pointerId, moved: false };
        tile.classList.add('dragging-ghost');
        try { tile.setPointerCapture(e.pointerId); } catch (_) {}
        return;
    }

    /* --- appui long 500 ms → entrée en mode édition --- */
    tile.classList.add('pressing');
    longPress = {
        tile, startX: e.clientX, startY: e.clientY, timer: null
    };
    longPress.timer = setTimeout(() => {
        tile.classList.remove('pressing');
        longPress = null;
        setEditMode(true);
        suppressNextClick = true;
        if (navigator.vibrate) { try { navigator.vibrate(20); } catch (_) {} }
    }, LONGPRESS_MS);
});

document.addEventListener('pointermove', (e) => {
    /* 1. Drag en cours : réordonnancement live de la grille */
    if (dragState) {
        if (e.pointerId !== dragState.pointerId) return;
        const tile = dragState.tile;
        /* elementFromPoint voit la tuile en capture : on la masque un instant */
        tile.style.visibility = 'hidden';
        const over = document.elementFromPoint(e.clientX, e.clientY);
        tile.style.visibility = '';
        const target = over ? over.closest('.nav-tile') : null;
        if (target && target !== tile && target.parentElement === navGrid) {
            dragState.moved = true;
            const tiles = Array.from(navGrid.querySelectorAll('.nav-tile'));
            if (tiles.indexOf(target) > tiles.indexOf(tile)) {
                navGrid.insertBefore(tile, target.nextSibling);
            } else {
                navGrid.insertBefore(tile, target);
            }
        }
        return;
    }
    /* 2. Appui long : annulé si le pointeur s'écarte de > 10 px */
    if (longPress) {
        const dx = e.clientX - longPress.startX;
        const dy = e.clientY - longPress.startY;
        if (dx * dx + dy * dy > 100) {
            clearTimeout(longPress.timer);
            longPress.tile.classList.remove('pressing');
            longPress = null;
        }
    }
});

function endPointer(e) {
    if (dragState && e.pointerId === dragState.pointerId) {
        const { tile, moved } = dragState;
        dragState = null;
        tile.classList.remove('dragging-ghost');
        if (moved) {
            suppressNextClick = true;
            saveDashboardOrder(navGrid);
        }
    }
    if (longPress) {
        clearTimeout(longPress.timer);
        longPress.tile.classList.remove('pressing');
        longPress = null;
    }
}
document.addEventListener('pointerup', endPointer);
document.addEventListener('pointercancel', endPointer);

/* ============================================================
   WEBSOCKET — télémétrie temps réel
   Règle de performance du socle : bufferiser les messages dans
   onmessage, ne toucher au DOM qu'une fois par rAF.
   ============================================================ */
let ws = null;
let wsRetryTimer = null;
let lastTele = null;
let rafPending = false;

function connectWS() {
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    ws = new WebSocket(proto + '://' + location.host + '/ws');

    ws.onopen = () => setBadge('badge-ws', 'WS: Connecté', 'badge-ok');
    ws.onclose = () => {
        setBadge('badge-ws', 'WS: Déconnecté', 'badge-off');
        clearTimeout(wsRetryTimer);
        wsRetryTimer = setTimeout(connectWS, WS_RECONNECT_MS);  /* reconnexion auto */
    };
    ws.onerror = () => { try { ws.close(); } catch (_) {} };
    ws.onmessage = (ev) => {
        let data;
        try { data = JSON.parse(ev.data); } catch (_) { return; }
        if (!data || data.type !== 'tele') return;
        lastTele = data;                    /* bufferisation — zéro DOM ici */
        if (!rafPending) {
            rafPending = true;
            requestAnimationFrame(renderTelemetry);
        }
    };
}

/* ===== RENDU TÉLÉMÉTRIE (1× par frame) ===== */
function renderTelemetry() {
    rafPending = false;
    if (!lastTele) return;
    const t = lastTele;

    /* Angles tête */
    setText($id('tl-yaw'), Number(t.yaw).toFixed(1));
    setText($id('tl-pitch'), Number(t.pitch).toFixed(1));
    setText($id('tl-roll'), Number(t.roll).toFixed(1));

    /* Joysticks : translation ±JOY_TRAVEL_PX proportionnelle à ±127 */
    const jx = clamp(Number(t.j1x) || 0, -JOY_RANGE, JOY_RANGE);
    const jy = clamp(Number(t.j1y) || 0, -JOY_RANGE, JOY_RANGE);
    const jz = clamp(Number(t.j2z) || 0, -JOY_RANGE, JOY_RANGE);
    setStyle($id('joy1-dot'), 'transform',
             'translate(' + (jx / JOY_RANGE * JOY_TRAVEL_PX).toFixed(1) + 'px,' +
             (jy / JOY_RANGE * JOY_TRAVEL_PX).toFixed(1) + 'px)');
    setStyle($id('joy2-dot'), 'transform',
             'translate(' + (jz / JOY_RANGE * JOY_TRAVEL_PX).toFixed(1) + 'px,0px)');
    setText($id('tl-j1x'), String(Math.round(jx)));
    setText($id('tl-j1y'), String(Math.round(jy)));
    setText($id('tl-j2z'), String(Math.round(jz)));

    /* Accélération linéaire : valeur + barre (pleine échelle 60 m/s²) */
    const lin = Number(t.lin) || 0;
    setText($id('tl-lin'), lin.toFixed(1));
    setStyle($id('lin-bar'), 'width',
             clamp(lin / LIN_MAX_MS2 * 100, 0, 100).toFixed(1) + '%');

    /* Bouton 1 (coup de tête sec) */
    const led = $id('btn1-led');
    const on = t.btn ? true : false;
    if (prevDom['btn1'] !== on) {
        prevDom['btn1'] = on;
        led.classList.toggle('on', on);
    }

    /* Badges + état BLE de la section Manette */
    const ble = t.ble ? true : false;
    const imu = t.imu ? true : false;
    setBadge('badge-ble', ble ? 'BLE: Connecté' : 'BLE: En attente', ble ? 'badge-ok' : 'badge-off');
    setBadge('badge-imu', imu ? 'IMU: OK' : 'IMU: ERREUR', imu ? 'badge-ok' : 'badge-err');
    setText($id('ble-state'),
            ble ? 'Connecté — rapports HID actifs' : 'En advertising — en attente d\u2019appairage');
}

/* ============================================================
   API MANETTE — réglages NVS + tare
   ============================================================ */
const gpInputs = {
    dzPitch: $id('in-dz-pitch'),
    dzRoll: $id('in-dz-roll'),
    dzYaw: $id('in-dz-yaw'),
    fullDeflectPitch: $id('in-full-pitch'),
    fullDeflectRoll: $id('in-full-roll'),
    fullDeflectYaw: $id('in-full-yaw'),
    jerkThresh: $id('in-jerk'),
    jerkCooldown: $id('in-cool'),
    invX: $id('ck-inv-x'),
    invY: $id('ck-inv-y'),
    invZ: $id('ck-inv-z')
};

function fillGamepadForm(cfg) {
    gpInputs.dzPitch.value = cfg.dzPitch;
    gpInputs.dzRoll.value = cfg.dzRoll;
    gpInputs.dzYaw.value = cfg.dzYaw;
    /* Repli legacy « fullDeflect » si le firmware n'envoie pas encore les
       trois axes distincts (flash LittleFS plus récent que le firmware) */
    gpInputs.fullDeflectPitch.value = cfg.fullDeflectPitch ?? cfg.fullDeflect;
    gpInputs.fullDeflectRoll.value = cfg.fullDeflectRoll ?? cfg.fullDeflect;
    gpInputs.fullDeflectYaw.value = cfg.fullDeflectYaw ?? cfg.fullDeflect;
    gpInputs.jerkThresh.value = cfg.jerkThresh;
    gpInputs.jerkCooldown.value = cfg.jerkCooldown;
    gpInputs.invX.checked = !!cfg.invX;
    gpInputs.invY.checked = !!cfg.invY;
    gpInputs.invZ.checked = !!cfg.invZ;
    /* Marqueur du seuil jerk sur la barre d'accélération */
    const th = clamp(Number(cfg.jerkThresh) / LIN_MAX_MS2 * 100, 0, 100);
    $id('lin-th').style.left = th.toFixed(1) + '%';
    setText($id('tl-th'), String(cfg.jerkThresh));
}

async function loadGamepadConfig() {
    try {
        const res = await fetch('/api/gamepad/config');
        if (!res.ok) throw new Error('HTTP ' + res.status);
        fillGamepadForm(await res.json());
    } catch (_) {
        setStatus('gp-status', 'Firmware injoignable — réessayez', 'err');
    }
}

async function saveGamepadConfig() {
    const body = new URLSearchParams();
    body.set('dzPitch', gpInputs.dzPitch.value);
    body.set('dzRoll', gpInputs.dzRoll.value);
    body.set('dzYaw', gpInputs.dzYaw.value);
    body.set('fullDeflectPitch', gpInputs.fullDeflectPitch.value);
    body.set('fullDeflectRoll', gpInputs.fullDeflectRoll.value);
    body.set('fullDeflectYaw', gpInputs.fullDeflectYaw.value);
    body.set('jerkThresh', gpInputs.jerkThresh.value);
    body.set('jerkCooldown', gpInputs.jerkCooldown.value);
    body.set('invX', gpInputs.invX.checked ? '1' : '0');
    body.set('invY', gpInputs.invY.checked ? '1' : '0');
    body.set('invZ', gpInputs.invZ.checked ? '1' : '0');

    const btn = $id('btn-gp-save');
    btn.disabled = true;
    setStatus('gp-status', 'Enregistrement…', '');
    try {
        const res = await fetch('/api/gamepad/config', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: body.toString()
        });
        const data = await res.json().catch(() => ({}));
        if (res.ok && data.status === 'ok') {
            fillGamepadForm(data);          /* valeurs normalisées par le firmware */
            setStatus('gp-status', 'Réglages enregistrés en NVS ✓', 'ok');
        } else {
            setStatus('gp-status', 'Erreur : ' + (data.error || 'inconnue'), 'err');
        }
    } catch (_) {
        setStatus('gp-status', 'Firmware injoignable — réessayez', 'err');
    } finally {
        btn.disabled = false;
    }
}

async function tareGamepad() {
    const btn = $id('btn-gp-tare');
    btn.disabled = true;
    setStatus('gp-status', 'Calibrage… placez la tête au neutre', '');
    try {
        const res = await fetch('/api/gamepad/tare', { method: 'POST' });
        const data = await res.json().catch(() => ({}));
        if (res.ok && data.status === 'ok') {
            setStatus('gp-status', (data.message || 'Point 0 recalibré') + ' ✓', 'ok');
        } else {
            setStatus('gp-status', 'Erreur : ' + (data.error || 'inconnue'), 'err');
        }
    } catch (_) {
        setStatus('gp-status', 'Firmware injoignable — réessayez', 'err');
    } finally {
        btn.disabled = false;
    }
}

$id('btn-gp-save').addEventListener('click', saveGamepadConfig);
$id('btn-gp-tare').addEventListener('click', tareGamepad);

/* ============================================================
   API BLUETOOTH — nom du périphérique (NVS)
   ============================================================ */
async function loadBleName() {
    try {
        const res = await fetch('/api/ble/name');
        if (!res.ok) throw new Error('HTTP ' + res.status);
        const data = await res.json();
        $id('in-ble-name').value = data.name;
        setText($id('ble-name'), data.name);
    } catch (_) {
        /* silencieux — nouvelle tentative à la prochaine ouverture */
    }
}

async function saveBleName() {
    const name = $id('in-ble-name').value.trim();
    if (!name) {
        setStatus('ble-status', 'Le nom ne peut pas être vide', 'err');
        return;
    }
    const btn = $id('btn-ble-save');
    btn.disabled = true;
    setStatus('ble-status', 'Enregistrement…', '');
    try {
        const body = new URLSearchParams();
        body.set('name', name);
        const res = await fetch('/api/ble/name', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: body.toString()
        });
        const data = await res.json().catch(() => ({}));
        if (res.ok && data.status === 'ok') {
            $id('in-ble-name').value = data.name;   /* valeur normalisée */
            setText($id('ble-name'), data.name);
            setStatus('ble-status', (data.message || 'Nom enregistré') + ' ✓', 'ok');
        } else {
            setStatus('ble-status', 'Erreur : ' + (data.error || 'inconnue'), 'err');
        }
    } catch (_) {
        setStatus('ble-status', 'Firmware injoignable — réessayez', 'err');
    } finally {
        btn.disabled = false;
    }
}

$id('btn-ble-save').addEventListener('click', saveBleName);
$id('in-ble-name').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') saveBleName();
});

/* ============================================================
   API WI-FI — scan + connexion STA
   ============================================================ */
let wifiScannedOnce = false;
let wifiSelected = null;

async function scanWifi() {
    const list = $id('wifi-list');
    const btn = $id('btn-wifi-scan');
    btn.disabled = true;
    list.innerHTML = '<div class="wifi-list-empty">Scan en cours…</div>';
    try {
        const res = await fetch('/api/wifi/scan');
        const data = await res.json().catch(() => ({}));
        if (!res.ok || data.error) {
            showWifiEmpty(data.error || ('Erreur ' + res.status));
        } else {
            renderWifiList(data.networks || []);
        }
        wifiScannedOnce = true;
    } catch (_) {
        showWifiEmpty('Scan impossible — vérifiez la liaison');
    } finally {
        btn.disabled = false;
    }
}

function showWifiEmpty(msg) {
    const list = $id('wifi-list');
    list.innerHTML = '';
    const el = document.createElement('div');
    el.className = 'wifi-list-empty';
    el.textContent = msg;
    list.appendChild(el);
}

function renderWifiList(networks) {
    const list = $id('wifi-list');
    list.innerHTML = '';
    if (!networks.length) {
        list.innerHTML = '<div class="wifi-list-empty">Aucun réseau trouvé</div>';
        return;
    }
    networks.forEach(net => {
        const item = document.createElement('div');
        item.className = 'wifi-item';
        /* Construction sûre (XSS) : structure statique + textContent */
        item.innerHTML =
            '<span class="wifi-ssid"></span>' +
            '<span class="wifi-meta">' +
            (net.secure ? '<span class="wifi-lock" title="Réseau protégé">🔒</span>' : '') +
            '<span class="wifi-rssi"></span></span>';
        item.querySelector('.wifi-ssid').textContent = net.ssid || '(réseau caché)';
        item.querySelector('.wifi-rssi').textContent = net.rssi + ' dBm';
        item.addEventListener('click', () => selectWifi(item, net.ssid || ''));
        list.appendChild(item);
    });
}

function selectWifi(item, ssid) {
    document.querySelectorAll('.wifi-item.selected')
        .forEach(el => el.classList.remove('selected'));
    item.classList.add('selected');
    wifiSelected = ssid;
    $id('wifi-pass-row').style.display = 'block';
    $id('wifi-ssid-label').textContent = ssid;
    $id('in-wifi-pass').focus();
}

async function connectWifi() {
    if (!wifiSelected) return;
    const btn = $id('btn-wifi-connect');
    btn.disabled = true;
    setStatus('wifi-status', 'Connexion en cours (jusqu\u2019à 15 s)…', '');
    const body = new URLSearchParams();
    body.set('ssid', wifiSelected);
    body.set('pass', $id('in-wifi-pass').value);
    try {
        const res = await fetch('/api/wifi/connect', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: body.toString()
        });
        const data = await res.json().catch(() => ({}));
        if (data.status === 'connected') {
            setStatus('wifi-status', 'Connecté au réseau client — IP : ' + data.ip, 'ok');
        } else {
            setStatus('wifi-status', 'Échec : ' + (data.message || data.error || 'vérifiez le mot de passe'), 'err');
        }
    } catch (_) {
        setStatus('wifi-status', 'Firmware injoignable — réessayez', 'err');
    } finally {
        btn.disabled = false;
    }
}

$id('btn-wifi-scan').addEventListener('click', scanWifi);
$id('btn-wifi-connect').addEventListener('click', connectWifi);
$id('in-wifi-pass').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') connectWifi();
});

/* ============================================================
   API SYSTÈME — statut + redémarrage
   ============================================================ */
let sysTimer = null;

function formatUptime(ms) {
    const s = Math.floor(ms / 1000);
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    const sec = s % 60;
    if (h > 0) return h + ' h ' + String(m).padStart(2, '0') + ' min';
    if (m > 0) return m + ' min ' + String(sec).padStart(2, '0') + ' s';
    return sec + ' s';
}

async function loadSystemStatus() {
    try {
        const res = await fetch('/api/system/status');
        const s = await res.json();

        setText($id('app-version'), s.version);
        setText($id('sys-version'), s.version);
        setText($id('sys-uptime'), formatUptime(s.uptimeMs || 0));
        setText($id('sys-heap'),
                Math.round((s.heapFree || 0) / 1024) + ' Ko (min ' +
                Math.round((s.heapMin || 0) / 1024) + ' Ko)');
        setText($id('sys-apip'),
                s.wifi.apIp + ' — ' + s.wifi.apClients + ' client(s) AP');

        let sta = 'non configuré';
        if (s.wifi.staState === 'connected') sta = s.wifi.staIp + ' (' + s.wifi.staSsid + ')';
        else if (s.wifi.staSsid) sta = s.wifi.staState + ' (' + s.wifi.staSsid + ')';
        setText($id('sys-staip'), sta);

        const imuEl = $id('sys-imu');
        imuEl.textContent = s.imu.ok
            ? 'OK — WHO_AM_I 0x' + s.imu.whoami + (s.imu.mag ? ' (magnétomètre OK)' : '')
            : 'Échec de détection';
        imuEl.className = 'info-value ' + (s.imu.ok ? 'good' : 'bad');

        const bleEl = $id('sys-ble');
        bleEl.textContent = s.ble.connected
            ? 'Connecté — ' + s.ble.clients + ' hôte(s)'
            : 'En advertising';
        bleEl.className = 'info-value ' + (s.ble.connected ? 'good' : '');

        /* Section Manette : nom + nombre d'hôtes */
        setText($id('ble-name'), s.ble.name);
        setText($id('ble-clients'), String(s.ble.clients));
    } catch (_) { /* silencieux — nouvelle tentative au prochain cycle */ }
}

function startSystemPolling() {
    stopSystemPolling();
    loadSystemStatus();
    sysTimer = setInterval(loadSystemStatus, SYS_POLL_MS);
}
function stopSystemPolling() {
    if (sysTimer) { clearInterval(sysTimer); sysTimer = null; }
}

$id('btn-restart').addEventListener('click', () => {
    if (!window.confirm('Redémarrer l\u2019appareil ?')) return;
    setStatus('sys-status', 'Redémarrage dans 3 s…', '');
    fetch('/api/system/restart', { method: 'POST' }).catch(() => {});
});

/* ============================================================
   DÉMARRAGE
   ============================================================ */
loadSystemStatus();   /* version du footer dès l'arrivée sur le dashboard */
connectWS();
