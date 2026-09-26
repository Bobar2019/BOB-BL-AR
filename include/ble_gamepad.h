#ifndef BLE_GAMEPAD_H
#define BLE_GAMEPAD_H

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <NimBLECharacteristic.h>

/* Bits du champ `buttons` du rapport Xbox — layout Mystfit/lemmingDev,
 * identique aux manettes réelles (les trous correspondent aux bits non
 * utilisés par le matériel d'origine, ils font partie du layout fil).
 * Le jerk est câblé sur A : bouton 0 du mapping « standard » de la
 * Web Gamepad API. */
constexpr uint16_t XBOX_BTN_A      = 0x0001;  /* bit 0  : A (jerk)        */
constexpr uint16_t XBOX_BTN_B      = 0x0002;  /* bit 1  : B               */
constexpr uint16_t XBOX_BTN_X      = 0x0008;  /* bit 3  : X               */
constexpr uint16_t XBOX_BTN_Y      = 0x0010;  /* bit 4  : Y               */
constexpr uint16_t XBOX_BTN_LB     = 0x0040;  /* bit 6  : gâchette gauche */
constexpr uint16_t XBOX_BTN_RB     = 0x0080;  /* bit 7  : gâchette droite */
constexpr uint16_t XBOX_BTN_SELECT = 0x0400;  /* bit 10 : Select          */
constexpr uint16_t XBOX_BTN_START  = 0x0800;  /* bit 11 : Start           */
constexpr uint16_t XBOX_BTN_GUIDE  = 0x1000;  /* bit 12 : Guide/Home      */
constexpr uint16_t XBOX_BTN_LS     = 0x2000;  /* bit 13 : click stick G   */
constexpr uint16_t XBOX_BTN_RS     = 0x4000;  /* bit 14 : click stick D   */

/** @brief Valeur « centré » des axes u16 du rapport Xbox. */
constexpr uint16_t XBOX_AXIS_CENTER = 0x8000;

#pragma pack(push, 1)
/**
 * @brief Rapport d'entrée HID n°1 « Xbox One S » (16 octets, layout fil).
 *
 *  - x / y   : stick gauche — X (roulis) et Y (tangage), u16 centré 0x8000
 *  - z / rz  : stick droit — Z (lacet) et Rz (réservé, maintenu centré)
 *  - brake / accelerator : gâchettes (relâchées : 0)
 *  - hat     : D-pad 4 bits (0 = neutre, 1..8 = Nord puis horaire)
 *  - buttons : bits XBOX_BTN_* — bit 0 (A) = jerk
 *  - share   : bouton Share/Record (0)
 *
 * Structure packée : le champ `buttons` (u16) vit à l'offset 13, non
 * aligné — GCC génère pour les membres d'une struct packée des accès
 * sûrs octet par octet sur le RISC-V du C6, qui autrement lèverait une
 * exception d'alignement. Sérialsée telle quelle (ESP32 little-endian).
 */
struct HidGamepadReport {
    uint16_t x;           /* stick gauche X — roulis                    */
    uint16_t y;           /* stick gauche Y — tangage                   */
    uint16_t z;           /* stick droit X — lacet                      */
    uint16_t rz;          /* stick droit Y — réservé (XBOX_AXIS_CENTER) */
    uint16_t brake;       /* gâchette gauche (0 = relâchée)             */
    uint16_t accelerator; /* gâchette droite (0 = relâchée)             */
    uint8_t  hat;         /* D-pad : 0 = neutre                         */
    uint16_t buttons;     /* bits XBOX_BTN_* (bit 0 = A = jerk)         */
    uint8_t  share;       /* bouton Share (0)                           */
};
#pragma pack(pop)
static_assert(sizeof(HidGamepadReport) == 16, "layout fil Xbox : 16 octets");

/**
 * @brief Handler d'un ordre de rumble reçu de l'hôte (Output Report n°3).
 *
 * Appelé dans le contexte de la tâche NimBLE : rester léger (aucune écriture
 * NVS ni appel bloquant — différer via un drapeau, cf. requestTare()).
 * N'est invoqué que pour les ordres non nuls (une des 4 magnitudes de
 * l'Output Report Xbox > 0 — moteurs fort/faible ou gâchettes), après
 * anti-rebond (BLE_RUMBLE_DEBOUNCE_MS).
 *
 * Magnitudes sur 0..100 (page PID) ; durée reconvertie en ms.
 */
using RumbleHandler = void (*)(uint8_t strongMagnitude, uint8_t weakMagnitude,
                                uint16_t durationMs);

/**
 * @brief Manette de jeu BLE HID-over-GATT construite sur NimBLE-Arduino 2.x.
 *
 * Rationale C6 : la bibliothèque BleGamepad (lemmingDev) v5.x repose sur
 * NimBLE-Arduino 1.x, incompatible ESP32-C6. Cette classe « équivalent C6 »
 * assemble le profil HID avec NimBLEHIDDevice :
 *  - Service HID (0x1812) + Report Map « Xbox One S 1708 » : rapport
 *    d'entrée 16 octets (sticks u16, gâchettes, D-pad, 15 boutons, Share),
 *    rapports annexes AC Home (0x02) et batterie (0x04), Output Report
 *    de rumble n°3 « Set Effect » (page PID 0x0F)
 *  - Identité PnP Xbox (0x045E:0x02FD) + manufacturer/serial Microsoft :
 *    condition pour que l'hôte lie son pilote manette natif — et que
 *    Chrome instancie gamepad.vibrationActuator (mapping « standard »)
 *  - Rapport d'entrée en notification (Report Reference n°1)
 *  - Rapport de sortie (Report Reference n°3, type Output) : canal
 *    hôte → manette pour le retour de force ; toute magnitude non nulle
 *    est notifiée via un RumbleHandler (ex. ordre de Tare)
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
     * @brief Enregistre le handler des ordres de rumble (Output Report n°3).
     *
     * L'hôte (Web Gamepad API / WebHID / hidapi) écrit le rapport
     * « Set Effect » Xbox {enables, gâchettes, faible, fort, durée…} :
     * toute magnitude non nulle est notifiée ici — canal de commande
     * natif BLE, sans passer par le Wi-Fi.
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
