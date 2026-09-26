#include "ble_gamepad.h"
#include "config.h"

BleGamepadHid g_bleGamepad;

/* ------------------------------------------------------------------ */
/* Descripteur de rapport HID — gamepad : X, Y, Z, Rz + 2 boutons      */
/*                                                                     */
/* Reconnu par macOS (GCController) et Windows (HID-compliant game     */
/* controller) : Usage Page Generic Desktop / Usage Game Pad.          */
/* Plage logique : −127..+127 (int8) par axe.                          */
/* ------------------------------------------------------------------ */
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop)      */
    0x09, 0x05,       /* Usage (Game Pad)                  */
    0xA1, 0x01,       /* Collection (Application)          */
    0x85, 0x01,       /*   Report ID (1)                   */
    0xA1, 0x00,       /*   Collection (Physical)           */
    0x09, 0x30,       /*     Usage (X)   — J1 gauche/droite (roulis)  */
    0x09, 0x31,       /*     Usage (Y)   — J1 avant/arrière (tangage) */
    0x09, 0x32,       /*     Usage (Z)   — J2 gauche/droite (lacet)   */
    0x09, 0x35,       /*     Usage (Rz)  — J2 avant/arrière (réservé) */
    0x15, 0x81,       /*     Logical Minimum (-127)        */
    0x25, 0x7F,       /*     Logical Maximum (127)         */
    0x75, 0x08,       /*     Report Size (8)               */
    0x95, 0x04,       /*     Report Count (4)              */
    0x81, 0x02,       /*     Input (Data, Var, Abs)        */
    0x05, 0x09,       /*     Usage Page (Button)           */
    0x19, 0x01,       /*     Usage Minimum (Bouton 1 : jerk) */
    0x29, 0x02,       /*     Usage Maximum (Bouton 2 : libre) */
    0x15, 0x00,       /*     Logical Minimum (0)           */
    0x25, 0x01,       /*     Logical Maximum (1)           */
    0x75, 0x01,       /*     Report Size (1)               */
    0x95, 0x02,       /*     Report Count (2)              */
    0x81, 0x02,       /*     Input (Data, Var, Abs)        */
    0x95, 0x01,       /*     Report Count (1)              */
    0x75, 0x06,       /*     Report Size (6)               */
    0x81, 0x03,       /*     Input (Const, Var, Abs) — padding */
    0xC0,             /*   End Collection (Physical)       */

    /* Output Report (1) : retour de force « dual-rumble » —             */
    /* strongMagnitude, weakMagnitude (0..255) puis durée (ms, u16 LE).  */
    /* Aucun moteur n'est piloté : toute magnitude non nulle est         */
    /* interprétée par le firmware comme un ordre de Tare (recentrage    */
    /* du point 0 de la tête). Format aligné sur l'effet « dual-rumble » */
    /* de la Web Gamepad API ; même Report ID que l'Input Report (un     */
    /* clavier partage ainsi ses rapports touches et LED).               */
    0x15, 0x00,       /*   Logical Minimum (0)             */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255)           */
    0x75, 0x08,       /*   Report Size (8)                 */
    0x95, 0x04,       /*   Report Count (4)                */
    0x91, 0x02,       /*   Output (Data, Var, Abs)         */
    0xC0              /* End Collection (Application)      */
};

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
    /* Charge utile « dual-rumble » : strongMagnitude, weakMagnitude puis
       durée en ms (u16 little endian). Selon la pile hôte, l'octet de
       Report ID préfixe la valeur écrite dans la caractéristique (usage
       de la spécification HID Service) — les deux formats sont acceptés. */
    const NimBLEAttValue value = pCharacteristic->getValue();
    const uint8_t* data = value.data();
    const size_t   len  = value.size();

    const size_t off = (len == 5 && data[0] == GAMEPAD_REPORT_ID) ? 1 : 0;
    if (len - off < 4) return;                     /* trame trop courte */

    const uint8_t  strong     = data[off];
    const uint8_t  weak       = data[off + 1];
    const uint16_t durationMs = (uint16_t)data[off + 2] | ((uint16_t)data[off + 3] << 8);

    /* Magnitudes nulles = arrêt de vibration : rien à notifier. */
    if (strong == 0 && weak == 0) return;

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

bool BleGamepadHid::begin() {
    NimBLEDevice::init(BLE_DEVICE_NAME);

    /* Appairage « Just Works » : bonding obligatoire (HID), pas de MITM
       (pas d'écran/clavier sur la manette), Secure Connections activé. */
    NimBLEDevice::setSecurityAuth(/*bonding*/ true, /*mitm*/ false, /*sc*/ true);

    _server = NimBLEDevice::createServer();
    _server->setCallbacks(new ServerCallbacks(this));
    _server->advertiseOnDisconnect(true);   /* re-advertising automatique */

    _hid = new NimBLEHIDDevice(_server);
    _hid->setReportMap(const_cast<uint8_t*>(HID_REPORT_MAP), sizeof(HID_REPORT_MAP));
    _hid->setHidInfo(0x00, 0x01);            /* pays 0, « normally connectable » */
    _hid->setPnp(0x02, BLE_VID, BLE_PID, BLE_VERSION);  /* vendor-assigned */
    _hid->setManufacturer(BLE_MANUFACTURER);
    _hid->setBatteryLevel(BLE_BATTERY_LEVEL);

    _input = _hid->getInputReport(GAMEPAD_REPORT_ID);

    /* Output Report (même Report ID, Report Reference type 0x02) : rumble
       hôte → manette. Le callback reste léger (parse + transfert) ; la
       politique — tare — est décidée en aval et exécutée par la tâche
       manette (aucune écriture NVS dans le contexte NimBLE). */
    _output = _hid->getOutputReport(GAMEPAD_REPORT_ID);
    _output->setCallbacks(new OutputCallbacks(this));

    _server->start();   /* démarre HID + Battery + Device Information */

    /* Advertising : service HID + apparence « gamepad » + réponse au scan */
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setAppearance(BLE_APPEARANCE_GAMEPAD);
    adv->addServiceUUID(_hid->getHidService()->getUUID());
    adv->enableScanResponse(true);
    NimBLEDevice::startAdvertising();

    Serial.println("[BLE] Manette « " + String(BLE_DEVICE_NAME) + " » en advertising");
    return true;
}

uint8_t BleGamepadHid::clientCount() const {
    return _server ? _server->getConnectedCount() : 0;
}

void BleGamepadHid::sendReport(const HidGamepadReport& report) {
    if (!_input || !_connected) return;

    uint8_t data[5];
    data[0] = (uint8_t)report.x1;
    data[1] = (uint8_t)report.y1;
    data[2] = (uint8_t)report.z2;
    data[3] = (uint8_t)report.rz2;
    data[4] = report.buttons;

    _input->setValue(data, sizeof(data));
    _input->notify();
}

void BleGamepadHid::setBatteryLevel(uint8_t percent) {
    if (_hid) _hid->setBatteryLevel(percent);
}
