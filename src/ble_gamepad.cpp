#include "ble_gamepad.h"
#include "config.h"
#include <Preferences.h>

BleGamepadHid g_bleGamepad;

/* ------------------------------------------------------------------ */
/* Descripteur de rapport HID — manette « Xbox One S » (modèle 1708)   */
/*                                                                     */
/* Transcription OCTET POUR OCTET du descripteur d'une manette Xbox    */
/* One S réelle (capture Mystfit/ESP32-BLE-CompositeHID, MIT — reprise */
/* et éprouvée en production par ESP32-BLE-Gamepad de lemmingDev).     */
/*                                                                     */
/* Ce layout — sticks u16 centrés 0x8000, gâchettes 10 bits, D-pad     */
/* 4 bits, 15 boutons + Share, et Output Report n°3 « Set Effect » de  */
/* la page d'usages PID (0x0F) — associé à l'identité PnP 0x045E:0x02FD*/
/* est ce que les piles manette des trois OS reconnaissent comme une   */
/* manette Xbox : c'est cette liaison de pilote qui déclenche le       */
/* rumble natif hôte → manette, et donc l'instanciation de             */
/* gamepad.vibrationActuator dans Chrome (mapping « standard » inclus).*/
/* NE PAS « corriger » la structure apparemment irrégulière (collection*/
/* AC Home imbriquée dans l'application Game Pad, rapports 0x02/0x04   */
/* non notifiés) : elle reflète la capture du matériel réel.           */
/* ------------------------------------------------------------------ */
static const uint8_t HID_REPORT_MAP[] = {
    /* ===== Rapport 0x01 (entrée principale, 16 octets) ===== */
    0x05, 0x01,       /* Usage Page (Generic Desktop)      */
    0x09, 0x05,       /* Usage (Game Pad)                  */
    0xA1, 0x01,       /* Collection (Application)          */
    0x85, 0x01,       /*   Report ID (1)                   */
    /* -- Stick gauche : X + Y, 2 × 16 bits (roulis, tangage) -- */
    0x09, 0x01,       /*   Usage (Pointer)                 */
    0xA1, 0x00,       /*   Collection (Physical)           */
    0x09, 0x30,       /*     Usage (X) — J1 g/d (roulis)   */
    0x09, 0x31,       /*     Usage (Y) — J1 a/p (tangage)  */
    0x15, 0x00,       /*     Logical Minimum (0)           */
    0x27, 0xFF, 0xFF, 0x00, 0x00, /* Logical Max (65535)    */
    0x95, 0x02,       /*     Report Count (2)              */
    0x75, 0x10,       /*     Report Size (16)              */
    0x81, 0x02,       /*     Input (Data, Var, Abs)        */
    0xC0,             /*   End Collection (Physical)       */
    /* -- Stick droit : Z + Rz, 2 × 16 bits (lacet, réservé) -- */
    0x09, 0x01,       /*   Usage (Pointer)                 */
    0xA1, 0x00,       /*   Collection (Physical)           */
    0x09, 0x32,       /*     Usage (Z) — J2 g/d (lacet)    */
    0x09, 0x35,       /*     Usage (Rz) — J2 a/p (réservé) */
    0x15, 0x00,       /*     Logical Minimum (0)           */
    0x27, 0xFF, 0xFF, 0x00, 0x00, /* Logical Max (65535)    */
    0x95, 0x02,       /*     Report Count (2)              */
    0x75, 0x10,       /*     Report Size (16)              */
    0x81, 0x02,       /*     Input (Data, Var, Abs)        */
    0xC0,             /*   End Collection (Physical)       */
    /* -- Gâchette gauche (Brake) : 10 bits + 6 bits de bourrage -- */
    0x05, 0x02,       /*   Usage Page (Simulation Ctrls)   */
    0x09, 0xC5,       /*   Usage (Brake)                   */
    0x15, 0x00,       /*   Logical Minimum (0)             */
    0x26, 0xFF, 0x03, /*   Logical Maximum (1023)          */
    0x95, 0x01,       /*   Report Count (1)                */
    0x75, 0x0A,       /*   Report Size (10)                */
    0x81, 0x02,       /*   Input (Data, Var, Abs)          */
    0x15, 0x00,       /*   Logical Minimum (0) — bourrage  */
    0x25, 0x00,       /*   Logical Maximum (0)             */
    0x75, 0x06,       /*   Report Size (6)                 */
    0x95, 0x01,       /*   Report Count (1)                */
    0x81, 0x03,       /*   Input (Const) — bourrage        */
    /* -- Gâchette droite (Accelerator) : 10 bits + 6 bits -- */
    0x05, 0x02,       /*   Usage Page (Simulation Ctrls)   */
    0x09, 0xC4,       /*   Usage (Accelerator)             */
    0x15, 0x00,       /*   Logical Minimum (0)             */
    0x26, 0xFF, 0x03, /*   Logical Maximum (1023)          */
    0x95, 0x01,       /*   Report Count (1)                */
    0x75, 0x0A,       /*   Report Size (10)                */
    0x81, 0x02,       /*   Input (Data, Var, Abs)          */
    0x15, 0x00,       /*   Logical Minimum (0) — bourrage  */
    0x25, 0x00,       /*   Logical Maximum (0)             */
    0x75, 0x06,       /*   Report Size (6)                 */
    0x95, 0x01,       /*   Report Count (1)                */
    0x81, 0x03,       /*   Input (Const) — bourrage        */
    /* -- D-pad : chapeau 4 bits (0 = neutre, 1..8 horaire) -- */
    0x05, 0x01,       /*   Usage Page (Generic Desktop)    */
    0x09, 0x39,       /*   Usage (Hat Switch)              */
    0x15, 0x01,       /*   Logical Minimum (1)             */
    0x25, 0x08,       /*   Logical Maximum (8)             */
    0x35, 0x00,       /*   Physical Minimum (0)            */
    0x46, 0x3B, 0x01, /*   Physical Maximum (315°)         */
    0x66, 0x14, 0x00, /*   Unit (degrés de rotation)       */
    0x75, 0x04,       /*   Report Size (4)                 */
    0x95, 0x01,       /*   Report Count (1)                */
    0x81, 0x42,       /*   Input (Data, Var, Null state)  */
    0x75, 0x04,       /*   Report Size (4) — bourrage      */
    0x95, 0x01,       /*   Report Count (1)                */
    0x15, 0x00,       /*   Logical Minimum (0)             */
    0x25, 0x00,       /*   Logical Maximum (0)             */
    0x35, 0x00,       /*   Physical Minimum (0)            */
    0x45, 0x00,       /*   Physical Maximum (0)            */
    0x65, 0x00,       /*   Unit (aucune)                   */
    0x81, 0x03,       /*   Input (Const) — bourrage        */
    /* -- Boutons 1..15 : 15 × 1 bit + 1 bit de bourrage --             */
    /*    bit0=A (jerk), bit1=B, bit3=X, bit4=Y, bit6=LB, bit7=RB,     */
    /*    bit10=Select, bit11=Start, bit12=Guide, bit13/14=clicks      */
    0x05, 0x09,       /*   Usage Page (Button)             */
    0x19, 0x01,       /*   Usage Minimum (Bouton 1 : A)    */
    0x29, 0x0F,       /*   Usage Maximum (Bouton 15)       */
    0x15, 0x00,       /*   Logical Minimum (0)             */
    0x25, 0x01,       /*   Logical Maximum (1)             */
    0x75, 0x01,       /*   Report Size (1)                 */
    0x95, 0x0F,       /*   Report Count (15)               */
    0x81, 0x02,       /*   Input (Data, Var, Abs)          */
    0x15, 0x00,       /*   Logical Minimum (0) — bourrage  */
    0x25, 0x00,       /*   Logical Maximum (0)             */
    0x75, 0x01,       /*   Report Size (1)                 */
    0x95, 0x01,       /*   Report Count (1)                */
    0x81, 0x03,       /*   Input (Const) — bourrage        */
    /* -- Slot Share : 1 bit + 7 bits — AC Back sur le 1708 -- */
    0x05, 0x0C,       /*   Usage Page (Consumer)           */
    0x0A, 0x24, 0x02, /*   Usage (AC Back)                 */
    0x15, 0x00,       /*   Logical Minimum (0)             */
    0x25, 0x01,       /*   Logical Maximum (1)             */
    0x95, 0x01,       /*   Report Count (1)                */
    0x75, 0x01,       /*   Report Size (1)                 */
    0x81, 0x02,       /*   Input (Data, Var, Abs)          */
    0x15, 0x00,       /*   Logical Minimum (0) — bourrage  */
    0x25, 0x00,       /*   Logical Maximum (0)             */
    0x75, 0x07,       /*   Report Size (7)                 */
    0x95, 0x01,       /*   Report Count (1)                */
    0x81, 0x03,       /*   Input (Const) — bourrage        */

    /* ===== Rapport 0x02 (entrée annexe, 1 octet : AC Home) — 1708 ===== */
    /* Caractéristique GATT créée mais JAMAIS notifiée : elle doit      */
    /* exister, sinon Windows rejette le service HID.                   */
    0x05, 0x0C,       /* Usage Page (Consumer)             */
    0x09, 0x01,       /* Usage (Consumer Control)          */
    0x85, 0x02,       /* Report ID (2)                     */
    0xA1, 0x01,       /* Collection (Application)          */
    0x05, 0x0C,       /*   Usage Page (Consumer)           */
    0x0A, 0x23, 0x02, /*   Usage (AC Home)                 */
    0x15, 0x00,       /*   Logical Minimum (0)             */
    0x25, 0x01,       /*   Logical Maximum (1)             */
    0x95, 0x01,       /*   Report Count (1)                */
    0x75, 0x01,       /*   Report Size (1)                 */
    0x81, 0x02,       /*   Input (Data, Var, Abs)          */
    0x15, 0x00,       /*   Logical Minimum (0) — bourrage  */
    0x25, 0x00,       /*   Logical Maximum (0)             */
    0x75, 0x07,       /*   Report Size (7)                 */
    0x95, 0x01,       /*   Report Count (1)                */
    0x81, 0x03,       /*   Input (Const) — bourrage        */
    0xC0,             /* End Collection (Application)      */

    /* ===== Rapport 0x03 (SORTIE, 8 octets : rumble « Set Effect ») ===== */
    /* Hôte → manette : enables 4 bits | gâchette G | gâchette D |      */
    /* moteur faible | moteur fort | durée | délai | boucle. C'est la   */
    /* trame qu'écrivent les piles manette des OS pour traduire         */
    /* playEffect('dual-rumble', …) — aucune magnitude n'est pilotée    */
    /* ici : toute valeur non nulle est un ordre de Tare.               */
    0x05, 0x0F,       /* Usage Page (PID — retour de force) */
    0x09, 0x21,       /* Usage (Set Effect Report)         */
    0x85, 0x03,       /* Report ID (3)                     */
    0xA1, 0x02,       /* Collection (Logical)              */
    0x09, 0x97,       /*   Usage (DC Enable Actuators)     */
    0x15, 0x00,       /*   Logical Minimum (0)             */
    0x25, 0x01,       /*   Logical Maximum (1)             */
    0x75, 0x04,       /*   Report Size (4)                 */
    0x95, 0x01,       /*   Report Count (1)                */
    0x91, 0x02,       /*   Output (Data, Var, Abs)         */
    0x15, 0x00,       /*   Logical Minimum (0) — bourrage  */
    0x25, 0x00,       /*   Logical Maximum (0)             */
    0x75, 0x04,       /*   Report Size (4)                 */
    0x95, 0x01,       /*   Report Count (1)                */
    0x91, 0x03,       /*   Output (Const) — bourrage       */
    0x09, 0x70,       /*   Usage (Magnitude) × 4           */
    0x15, 0x00,       /*   Logical Minimum (0)             */
    0x25, 0x64,       /*   Logical Maximum (100)           */
    0x75, 0x08,       /*   Report Size (8)                 */
    0x95, 0x04,       /*   Report Count (4) : gG, gD, faible, fort */
    0x91, 0x02,       /*   Output (Data, Var, Abs)         */
    0x09, 0x50,       /*   Usage (Duration)                */
    0x66, 0x01, 0x10, /*   Unit (secondes)                 */
    0x55, 0x0E,       /*   Unit Exponent (-2 → 10 ms)      */
    0x15, 0x00,       /*   Logical Minimum (0)             */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255)           */
    0x75, 0x08,       /*   Report Size (8)                 */
    0x95, 0x01,       /*   Report Count (1)                */
    0x91, 0x02,       /*   Output (Data, Var, Abs)         */
    0x09, 0xA7,       /*   Usage (Start Delay)             */
    0x15, 0x00,       /*   Logical Minimum (0)             */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255)           */
    0x75, 0x08,       /*   Report Size (8)                 */
    0x95, 0x01,       /*   Report Count (1)                */
    0x91, 0x02,       /*   Output (Data, Var, Abs)         */
    0x65, 0x00,       /*   Unit (aucune)                   */
    0x55, 0x00,       /*   Unit Exponent (0)               */
    0x09, 0x7C,       /*   Usage (Loop Count)              */
    0x15, 0x00,       /*   Logical Minimum (0)             */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255)           */
    0x75, 0x08,       /*   Report Size (8)                 */
    0x95, 0x01,       /*   Report Count (1)                */
    0x91, 0x02,       /*   Output (Data, Var, Abs)         */
    0xC0,             /* End Collection (Logical)          */

    /* ===== Rapport 0x04 (entrée annexe, 1 octet : batterie) — 1708 ===== */
    /* Initialisé au niveau de batterie, jamais notifié (le service    */
    /* Battery 0x180F assure l'essentiel).                             */
    0x05, 0x06,       /* Usage Page (Generic Dev Controls) */
    0x09, 0x20,       /* Usage (Battery Strength)          */
    0x85, 0x04,       /* Report ID (4)                     */
    0x15, 0x00,       /* Logical Minimum (0)               */
    0x26, 0xFF, 0x00, /* Logical Maximum (255)             */
    0x75, 0x08,       /* Report Size (8)                   */
    0x95, 0x01,       /* Report Count (1)                  */
    0x81, 0x02,       /* Input (Data, Var, Abs)            */
    0xC0              /* End Collection (Application)      */
};

/* Rapports annexes du descripteur 1708 (implémentation, hors config.h). */
constexpr uint8_t GAMEPAD_ACHOME_REPORT_ID  = 0x02;  /* AC Home          */
constexpr uint8_t GAMEPAD_BATTERY_REPORT_ID = 0x04;  /* Battery Strength */

/* ------------------------------------------------------------------ */
/* Callbacks serveur                                                   */
/* ------------------------------------------------------------------ */

void BleGamepadHid::ServerCallbacks::onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) {
    _owner->_connected = true;
    Serial.println("[BLE] Hôte connecté : " + String(connInfo.getAddress().toString().c_str()));
    (void)server;
}

void BleGamepadHid::ServerCallbacks::onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) {
    _owner->_connected = false;
    Serial.println("[BLE] Hôte déconnecté (raison " + String(reason) + ") — advertising relancé");
    (void)server;
}

void BleGamepadHid::ServerCallbacks::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
    Serial.println("[BLE] Appairage terminé avec " + String(connInfo.getAddress().toString().c_str()));
}

/* ------------------------------------------------------------------ */
/* Réception de l'Output Report (rumble / retour de force)             */
/* ------------------------------------------------------------------ */

void BleGamepadHid::OutputCallbacks::onWrite(NimBLECharacteristic* pCharacteristic,
                                             NimBLEConnInfo& connInfo) {
    /* Output Report Xbox n°3 (8 octets, « Set Effect Report », page PID) :
       enables 4 bits | gâchette G | gâchette D | moteur faible | moteur
       fort | durée (unités de 10 ms) | délai | boucle. C'est cette trame
       qu'écrivent les piles manette des OS pour traduire
       playEffect('dual-rumble', …) de la Web Gamepad API. Selon la pile
       hôte, l'octet de Report ID préfixe la valeur écrite dans la
       caractéristique (usage de la spécification HID Service) — les deux
       formats sont acceptés. */
    const NimBLEAttValue value = pCharacteristic->getValue();
    const uint8_t* data = value.data();
    const size_t   len  = value.size();

    const size_t off = (len == 9 && data[0] == GAMEPAD_RUMBLE_REPORT_ID) ? 1 : 0;
    if (len - off < 8) return;                     /* trame trop courte */

    const uint8_t  trigL      = data[off + 1];     /* gâchette gauche  */
    const uint8_t  trigR      = data[off + 2];     /* gâchette droite  */
    const uint8_t  weak       = data[off + 3];     /* moteur faible    */
    const uint8_t  strong     = data[off + 4];     /* moteur fort      */
    const uint16_t durationMs = (uint16_t)data[off + 5] * 10;  /* 10 ms */

    /* Magnitudes nulles = arrêt de vibration : rien à notifier. Les
       gâchettes comptent aussi : certains effets de l'hôte (GCController)
       ne sollicitent qu'elles. */
    if (strong == 0 && weak == 0 && trigL == 0 && trigR == 0) return;

    /* Anti-rebond : un effet continu relancé périodiquement par l'hôte ne
       déclenche qu'une seule tare par fenêtre (cf. BLE_RUMBLE_DEBOUNCE_MS). */
    const uint32_t nowMs = millis();
    if (nowMs - _owner->_lastRumbleMs < BLE_RUMBLE_DEBOUNCE_MS) return;
    _owner->_lastRumbleMs = nowMs;

    Serial.println("[BLE] Ordre de rumble : fort=" + String(strong) +
                   ", faible=" + String(weak) + ", durée=" + String(durationMs) + " ms");

    /* Le handler reste volontairement léger (contexte de la tâche NimBLE) :
       la politique — tare — est décidée en aval (main.cpp) et exécutée par
       la tâche manette 100 Hz, jamais ici. */
    if (_owner->_rumbleHandler) {
        _owner->_rumbleHandler(strong, weak, durationMs);
    }
    (void)connInfo;
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                      */
/* ------------------------------------------------------------------ */

/* Nom Bluetooth : NVS « ble »/« name », repli sur la constante. La clé
   n'existe pas au premier flash — Preferences renvoie alors le défaut. */
void BleGamepadHid::_loadDeviceName() {
    Preferences prefs;
    if (prefs.begin("ble", true)) {
        _deviceName = prefs.getString("name", BLE_DEVICE_NAME);
        prefs.end();
    } else {
        _deviceName = BLE_DEVICE_NAME;
    }
    if (_deviceName.length() < 1 || _deviceName.length() > BLE_NAME_MAX_LEN) {
        _deviceName = BLE_DEVICE_NAME;   /* valeur corrompue → défaut */
    }
}

bool BleGamepadHid::begin() {
    _loadDeviceName();
    NimBLEDevice::init(_deviceName.c_str());

    /* Appairage « Just Works » : bonding obligatoire (HID), pas de MITM
       (pas d'écran/clavier sur la manette), Secure Connections activé. */
    NimBLEDevice::setSecurityAuth(/*bonding*/ true, /*mitm*/ false, /*sc*/ true);

    _server = NimBLEDevice::createServer();
    _server->setCallbacks(new ServerCallbacks(this));
    _server->advertiseOnDisconnect(true);   /* re-advertising automatique */

    _hid = new NimBLEHIDDevice(_server);
    _hid->setReportMap(const_cast<uint8_t*>(HID_REPORT_MAP), sizeof(HID_REPORT_MAP));
    _hid->setHidInfo(0x00, 0x01);            /* pays 0, « normally connectable » */
    /* Identité Xbox One S : c'est le triplet PnP (0x045E:0x02FD) +
       descripteur du protocole Xbox qui fait lier à l'hôte son pilote
       manette natif — condition pour que Chrome instancie
       gamepad.vibrationActuator (GCController haptics sur macOS 14+,
       pilote Xbox sur Windows, hid-microsoft sur Linux). */
    _hid->setPnp(0x02, BLE_VID, BLE_PID, BLE_VERSION);  /* vendor-assigned */
    _hid->setManufacturer(BLE_MANUFACTURER);
    _hid->setBatteryLevel(BLE_BATTERY_LEVEL);

    /* Numéro de série (0x2A25) d'une manette One S réelle — le pilote
       Xbox de l'hôte l'utilise pour le matching. */
    NimBLECharacteristic* serial = _hid->getDeviceInfoService()->createCharacteristic(
        NimBLEUUID((uint16_t)0x2A25), NIMBLE_PROPERTY::READ);
    serial->setValue(BLE_SERIAL_NUMBER);

    _input = _hid->getInputReport(GAMEPAD_REPORT_ID);

    /* Rapports annexes du descripteur 1708 : AC Home (0x02) — la
       caractéristique doit exister sinon Windows rejette le service —
       et batterie (0x04), initialisé mais jamais notifié. */
    (void)_hid->getInputReport(GAMEPAD_ACHOME_REPORT_ID);
    const uint8_t batteryLevel = BLE_BATTERY_LEVEL;
    _hid->getInputReport(GAMEPAD_BATTERY_REPORT_ID)->setValue(&batteryLevel, 1);

    /* Output Report de rumble (Report ID 0x03, Report Reference type
       0x02) : hôte → manette. Le callback reste léger (parse +
       transfert) ; la politique — tare — est décidée en aval et exécutée
       par la tâche manette (aucune écriture NVS dans le contexte NimBLE). */
    _output = _hid->getOutputReport(GAMEPAD_RUMBLE_REPORT_ID);
    _output->setCallbacks(new OutputCallbacks(this));

    _server->start();   /* démarre HID + Battery + Device Information */

    /* Advertising : service HID + apparence « gamepad » + nom complet dans
       la scan response (visible au scan AVANT connexion) + réponse au scan */
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setAppearance(BLE_APPEARANCE_GAMEPAD);
    adv->addServiceUUID(_hid->getHidService()->getUUID());
    adv->setName(_deviceName.c_str());
    adv->enableScanResponse(true);
    NimBLEDevice::startAdvertising();

    Serial.println("[BLE] Manette « " + _deviceName + " » en advertising");
    return true;
}

uint8_t BleGamepadHid::clientCount() const {
    return _server ? _server->getConnectedCount() : 0;
}

void BleGamepadHid::sendReport(const HidGamepadReport& report) {
    if (!_input || !_connected) return;

    /* Structure packée de 16 octets sérialisée telle quelle : l'ESP32-C6
       est little-endian, conforme au layout fil Xbox (static_assert dans
       ble_gamepad.h). */
    _input->setValue(reinterpret_cast<const uint8_t*>(&report), sizeof(report));
    _input->notify();
}

void BleGamepadHid::setBatteryLevel(uint8_t percent) {
    if (_hid) _hid->setBatteryLevel(percent);
}

/* ------------------------------------------------------------------ */
/* Renommage (interface web → POST /api/ble/name)                       */
/* ------------------------------------------------------------------ */

bool BleGamepadHid::setDeviceName(const String& name) {
    String n = name;
    n.trim();
    if (n.length() < 1 || n.length() > BLE_NAME_MAX_LEN) return false;

    /* NVS + relecture de confirmation (convention du socle) */
    Preferences prefs;
    if (!prefs.begin("ble", false)) return false;
    prefs.putString("name", n);
    const bool ok = prefs.getString("name", "") == n;
    prefs.end();
    if (!ok) return false;

    _deviceName = n;

    /* Application à chaud : caractéristique GAP Device Name (0x2A00) puis
       payload d'advertising (scan response). Si un hôte est connecté,
       l'advertising est inactif — advertiseOnDisconnect le relancera avec
       la nouvelle payload ; les hôtes déjà appairés peuvent néanmoins
       conserver l'ancien nom en cache (ré-appairage nécessaire). */
    NimBLEDevice::setDeviceName(n.c_str());
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setName(n.c_str());
    if (!_connected) {
        adv->stop();
        adv->start();
    }

    Serial.println("[BLE] Manette renommée « " + n + " »");
    return true;
}
