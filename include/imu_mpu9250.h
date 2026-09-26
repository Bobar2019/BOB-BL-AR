#ifndef IMU_MPU9250_H
#define IMU_MPU9250_H

#include <Arduino.h>
#include <Wire.h>
#include "config.h"

/**
 * @brief Driver I2C du capteur inertiel MPU9250 (accéléro + gyro + magnétomètre
 *        AK8963 intégré, accès en mode « bypass »).
 *
 * Configuration appliquée dans begin() :
 *  - Accéléromètre ±8 g  (4096 LSB/g)
 *  - Gyroscope     ±1000 °/s (32.8 LSB/(°/s))
 *  - DLPF 41 Hz, échantillonnage 100 Hz (SRD = 9)
 *  - Magnétomètre AK8963 : 16 bits, 100 Hz, correction ASA (fuse ROM)
 *
 * Le biais gyroscopique est estimé au démarrage (capteur immobile) puis
 * persisté en NVS (namespace « imu ») pour les démarrages en mouvement.
 */
class Mpu9250 {
public:
    /**
     * @brief Initialise le bus I2C et le capteur.
     * @param autoCalibGyro true : calibrage du biais gyro au démarrage
     *                      (l'appareil doit rester immobile ~2 s)
     * @return false si le MPU9250 est introuvable sur le bus
     */
    bool begin(bool autoCalibGyro = true);

    /**
     * @brief Lit une mesure complète.
     * @param accelG  [sortie] accélération en g (pesanteur comprise)
     * @param gyroDps [sortie] vitesse angulaire en °/s (biais retiré)
     * @param magUt   [sortie] champ magnétique en µT (0 si indisponible)
     * @param tempC   [sortie, optionnel] température interne en °C
     * @return false en cas d'échec de transaction I2C
     */
    bool readAll(float accelG[3], float gyroDps[3], float magUt[3], float* tempC = nullptr);

    /**
     * @brief Calibre le biais gyroscopique (capteur strictement immobile).
     *
     * Moyenne de IMU_GYRO_CALIB_SAMPLES échantillons ; un écart-type trop
     * élevé (mouvement détecté) fait échouer le calibrage.
     * @return true si le biais a été mesuré et persisté en NVS
     */
    bool calibrateGyro();

    /** @brief Recharge le biais gyroscopique persisté en NVS */
    bool loadGyroBias();

    /** @brief Capteur détecté et initialisé */
    bool ok() const { return _ok; }

    /** @brief Magnétomètre AK8963 détecté */
    bool magOk() const { return _magOk; }

    /** @brief Valeur du registre WHO_AM_I (0x71 = MPU9250, 0x73 = MPU9255,
     *         0x70 = MPU6500, 0x68 = MPU6050 — clones compatibles acceptés) */
    uint8_t whoAmI() const { return _whoami; }

    /** @brief Adresse I2C du capteur détectée au démarrage (0x68 ou 0x69) */
    uint8_t address() const { return _addr; }

    /** @brief Température interne du dernier readAll() (°C) */
    float lastTempC() const { return _tempC; }

private:
    bool _write8(uint8_t dev, uint8_t reg, uint8_t val);
    bool _read8(uint8_t dev, uint8_t reg, uint8_t& val);
    bool _readBytes(uint8_t dev, uint8_t reg, uint8_t* buf, uint8_t n);
    bool _initMag();

    /** @brief Diagnostique de bus : log toutes les adresses qui répondent
     *         (0 périphérique = câblage ; toutes = bus bloqué SDA/SCL). */
    void _scanBus();

    bool _ok = false;
    bool _magOk = false;
    uint8_t _whoami = 0;
    uint8_t _addr = IMU_I2C_ADDR;   /* 0x68 ou 0x69 selon AD0 */
    float _gyroBias[3] = {0.0f, 0.0f, 0.0f};
    float _magAdj[3] = {1.0f, 1.0f, 1.0f}; /* corrections ASA AK8963 */
    float _tempC = 0.0f;
};

/** @brief Instance globale du capteur (définie dans imu_mpu9250.cpp) */
extern Mpu9250 g_imu;

#endif /* IMU_MPU9250_H */
