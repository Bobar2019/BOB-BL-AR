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
 * @brief Manette de jeu BLE HID-over-GATT construite sur NimBLE-Arduino 2.x.
 *
 * Rationale C6 : la bibliothèque BleGamepad (lemmingDev) v5.x repose sur
 * NimBLE-Arduino 1.x, incompatible ESP32-C6. Cette classe « équivalent C6 »
 * assemble le profil HID standard avec NimBLEHIDDevice :
 *  - Service HID (0x1812) + Report Map (gamepad 2 joysticks + 2 boutons)
 *  - Rapport d'entrée en notification (Report Reference n°1)
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

    NimBLEServer*        _server = nullptr;
    NimBLEHIDDevice*     _hid = nullptr;
    NimBLECharacteristic* _input = nullptr;
    volatile bool        _connected = false;
};

/** @brief Instance globale de la manette (définie dans ble_gamepad.cpp) */
extern BleGamepadHid g_bleGamepad;

#endif /* BLE_GAMEPAD_H */
