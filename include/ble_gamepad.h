#ifndef BLE_GAMEPAD_H
#define BLE_GAMEPAD_H

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <NimBLECharacteristic.h>

/**
 * @brief Rapport HID de la manette (5 octets, rapport n°1).
 *
 *  - x1 / y1 : Joystick 1 — X (roulis) et Y (tangage)
 *  - z2      : Joystick 2 — Z (lacet)
 *  - rz2     : Joystick 2 — Rz (réservé, maintenu à 0)
 *  - buttons : bit0 = Bouton 1 (jerk), bit1 = Bouton 2 (libre)
 */
struct HidGamepadReport {
    int8_t  x1;
    int8_t  y1;
    int8_t  z2;
    int8_t  rz2;
    uint8_t buttons;
};

/**
 * @brief Handler d'un ordre de rumble reçu de l'hôte (Output Report HID).
 *
 * Appelé dans le contexte de la tâche NimBLE : rester léger (aucune écriture
 * NVS ni appel bloquant — différer via un drapeau, cf. requestTare()).
 * N'est invoqué que pour les ordres non nuls (strongMagnitude ou
 * weakMagnitude > 0), après anti-rebond (BLE_RUMBLE_DEBOUNCE_MS).
 */
using RumbleHandler = void (*)(uint8_t strongMagnitude, uint8_t weakMagnitude,
                                uint16_t durationMs);

/**
 * @brief Manette de jeu BLE HID-over-GATT construite sur NimBLE-Arduino 2.x.
 *
 * Rationale C6 : la bibliothèque BleGamepad (lemmingDev) v5.x repose sur
 * NimBLE-Arduino 1.x, incompatible ESP32-C6. Cette classe « équivalent C6 »
 * assemble le profil HID standard avec NimBLEHIDDevice :
 *  - Service HID (0x1812) + Report Map (gamepad 2 joysticks + 2 boutons)
 *  - Rapport d'entrée en notification (Report Reference n°1)
 *  - Rapport de sortie « dual-rumble » (Report Reference n°1, type Output) :
 *    canal hôte → manette pour le retour de force ; toute magnitude non
 *    nulle est notifiée via un RumbleHandler (ex. ordre de Tare)
 *  - Service Battery (0x180F) et Device Information (PnP, manufacturer)
 *  - Appairage « Just Works » avec bonding + Secure Connections (macOS/Windows)
 */
class BleGamepadHid {
public:
    /** @brief Initialise NimBLE, les services HID et l'advertising. */
    bool begin();

    /** @brief true si au moins un hôte (macOS/Windows) est connecté. */
    bool isConnected() const { return _connected; }

    /** @brief Nombre d'hôtes actuellement connectés. */
    uint8_t clientCount() const;

    /**
     * @brief Envoie un rapport d'entrée en notification (si connecté).
     * Thread-safe : appelée depuis la tâche manette à 100 Hz.
     */
    void sendReport(const HidGamepadReport& report);

    /**
     * @brief Enregistre le handler des ordres de rumble (Output Report).
     *
     * L'hôte (Web Gamepad API / WebHID / hidapi) écrit un rapport
     * « dual-rumble » {strongMagnitude, weakMagnitude, durée} : toute
     * magnitude non nulle est notifiée ici — canal de commande natif
     * BLE, sans passer par le Wi-Fi.
     */
    void setRumbleHandler(RumbleHandler handler) { _rumbleHandler = handler; }

    /** @brief Nom Bluetooth courant (NVS « ble »/name, défaut BLE_DEVICE_NAME). */
    String deviceName() const { return _deviceName; }

    /**
     * @brief Renomme la manette : persiste en NVS puis applique à chaud.
     *
     * Validation : 1 à BLE_NAME_MAX_LEN octets (après trim). Application :
     * caractéristique GAP Device Name (0x2A00) + payload d'advertising
     * (scan response), advertising relancé si aucun hôte n'est connecté.
     * @return true si la NVS est confirmée par relecture.
     */
    bool setDeviceName(const String& name);

    /** @brief Met à jour le niveau de batterie annoncé. */
    void setBatteryLevel(uint8_t percent);

private:
    /** @brief Callbacks serveur : connexion / déconnexion / appairage. */
    class ServerCallbacks : public NimBLEServerCallbacks {
    public:
        explicit ServerCallbacks(BleGamepadHid* owner) : _owner(owner) {}
        void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override;
        void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override;
        void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;
    private:
        BleGamepadHid* _owner;
    };

    /** @brief Callbacks de l'Output Report : rumble / retour de force. */
    class OutputCallbacks : public NimBLECharacteristicCallbacks {
    public:
        explicit OutputCallbacks(BleGamepadHid* owner) : _owner(owner) {}
        void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override;
    private:
        BleGamepadHid* _owner;
    };

    NimBLEServer*        _server = nullptr;
    NimBLEHIDDevice*     _hid = nullptr;
    NimBLECharacteristic* _input = nullptr;
    NimBLECharacteristic* _output = nullptr;      /* Output Report (rumble) */
    RumbleHandler        _rumbleHandler = nullptr;
    uint32_t             _lastRumbleMs = 0;       /* anti-rebond des ordres */
    volatile bool        _connected = false;

    /** @brief Charge le nom Bluetooth depuis la NVS (défaut : constante). */
    void _loadDeviceName();

    String                _deviceName;            /* nom d'appairage courant */
};

/** @brief Instance globale de la manette (définie dans ble_gamepad.cpp) */
extern BleGamepadHid g_bleGamepad;

#endif /* BLE_GAMEPAD_H */
