#ifndef HEAD_TRACKER_H
#define HEAD_TRACKER_H

#include <Arduino.h>
#include <freertos/semphr.h>
#include "config.h"
#include "ahrs_mahony.h"
#include "imu_mpu9250.h"
#include "ble_gamepad.h"

/** @brief Paramètres de mapping modifiables depuis l'interface web (persistés NVS). */
struct GamepadSettings {
    float    deadzonePitchDeg  = GP_DEFAULT_DEADZONE_PITCH;   /**< Zone morte tangage (°) */
    float    deadzoneRollDeg   = GP_DEFAULT_DEADZONE_ROLL;    /**< Zone morte roulis (°) */
    float    deadzoneYawDeg    = GP_DEFAULT_DEADZONE_YAW;     /**< Zone morte lacet (°) */
    float    fullDeflectPitchDeg = GP_DEFAULT_FULL_DEFLECT;   /**< Pleine déviation tangage (°) */
    float    fullDeflectRollDeg  = GP_DEFAULT_FULL_DEFLECT;   /**< Pleine déviation roulis (°) */
    float    fullDeflectYawDeg   = GP_DEFAULT_FULL_DEFLECT;   /**< Pleine déviation lacet (°) */
    float    jerkThresholdMs2  = GP_DEFAULT_JERK_THRESHOLD;   /**< Seuil jerk (m/s²) */
    uint16_t jerkCooldownMs    = GP_DEFAULT_JERK_COOLDOWN;    /**< Recharge Bouton 1 (ms) */
    bool     invertX = false;   /**< Inverser l'axe X (roulis) */
    bool     invertY = false;   /**< Inverser l'axe Y (tangage) */
    bool     invertZ = false;   /**< Inverser l'axe Z (lacet) */
};

/** @brief Instantané télémétrie (WebSocket, 10 Hz). */
struct TrackerTelemetry {
    float yawDeg = 0.0f;        /**< Lacet taré (°) — droite positif */
    float pitchDeg = 0.0f;      /**< Tangage taré (°) — nez haut positif */
    float rollDeg = 0.0f;       /**< Roulis taré (°) — droite positif */
    float linAccMs2 = 0.0f;     /**< Norme de l'accélération linéaire filtrée (m/s²) */
    float j1x = 0.0f;           /**< Sortie Joystick 1 X, −127..+127 */
    float j1y = 0.0f;           /**< Sortie Joystick 1 Y, −127..+127 */
    float j2z = 0.0f;           /**< Sortie Joystick 2 Z, −127..+127 */
    bool  btn1 = false;         /**< Bouton 1 (jerk) actif */
    bool  imuOk = false;        /**< Capteur opérationnel */
    bool  bleConnected = false; /**< Hôte BLE connecté */
    uint32_t i2cErrors = 0;     /**< Erreurs I2C cumulées */
};

/**
 * @brief Cœur métier : tête → manette.
 *
 * Tâche FreeRTOS à 100 Hz (priorité supérieure au serveur web) :
 *  1. lecture du MPU9250 (I2C) ;
 *  2. fusion Mahony 6 axes → yaw / pitch / roll ;
 *  3. retrait de la pesanteur → accélération linéaire → détection de jerk ;
 *  4. application des zones mortes + sensibilité → axes HID (±127) ;
 *  5. envoi du rapport BLE HID si changement (débit limité).
 *
 * Calibrage :
 *  - biais gyroscopique : automatique au démarrage (immobile) ou NVS ;
 *  - « tare » : le point 0 de la tête (offsets d'orientation), déclenchable
 *    depuis l'interface web, persisté en NVS.
 */
class HeadTracker {
public:
    /** @brief Charge les réglages NVS puis initialise le capteur. */
    bool begin();

    /** @brief Crée la tâche temps réel (appelé après BLE + web). */
    void startTask();

    /**
     * @brief Recentre le point 0 de la tête (position actuelle = neutre).
     * @return true si les offsets ont été persistés en NVS.
     */
    bool tare();

    /**
     * @brief Demande une tare depuis un contexte étranger (callback BLE).
     *
     * L'appel reste léger (simple drapeau) : la tare complète — copie des
     * angles bruts dans les offsets et écriture NVS — est exécutée par la
     * tâche manette 100 Hz, jamais dans le contexte de l'appelant.
     * Utilisé par le canal de commande rumble (Output Report HID), cf.
     * docs/TARE_FORCE_FEEDBACK.md.
     */
    void requestTare() { _tareRequested = true; }

    /** @brief Réglages courants (lecture seule, pour l'API REST). */
    const GamepadSettings& settings() const { return _settings; }

    /**
     * @brief Applique des réglages (bornés) à chaud et les persiste en NVS.
     * @return true si l'écriture NVS est confirmée par relecture.
     */
    bool applySettings(const GamepadSettings& s);

    /** @brief Copie protégée du dernier instantané télémétrie. */
    TrackerTelemetry telemetry();

    /** @brief Capteur opérationnel. */
    bool imuOk() const { return _imuOk; }

private:
    static void _taskEntry(void* param);
    void _taskLoop();

    /** @brief Convertit un angle (°) en déviation d'axe −127..+127.
     *
     * La pleine déviation est propre à chaque axe (sensibilités indépendantes). */
    float _mapAngle(float angleDeg, float deadzoneDeg, float fullDeflectDeg) const;

    void _loadSettings();
    bool _saveSettings();

    GamepadSettings  _settings;
    TrackerTelemetry _tele;
    SemaphoreHandle_t _mutex = nullptr;

    MahonyAhrs _ahrs;
    bool _imuOk = false;

    /* Accès léger depuis la tâche web (écrits alignés 32 bits) */
    volatile float _rawPitchDeg = 0.0f;
    volatile float _rawRollDeg = 0.0f;
    volatile float _rawYawDeg = 0.0f;

    /* Tare demandée hors tâche manette (callback BLE : ordre de rumble) */
    volatile bool _tareRequested = false;

    float _tarePitch = 0.0f;
    float _tareRoll = 0.0f;
    float _tareYaw = 0.0f;

    float _linAccFilt = 0.0f;
    bool  _btnActive = false;
    uint32_t _lastJerkMs = 0;
};

/** @brief Instance globale (définie dans head_tracker.cpp) */
extern HeadTracker g_headTracker;

#endif /* HEAD_TRACKER_H */
