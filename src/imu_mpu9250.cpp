#include "imu_mpu9250.h"
#include "config.h"
#include <Preferences.h>

/* ---- Registres MPU9250 ---- */
static constexpr uint8_t MPU_WHO_AM_I     = 0x75;
static constexpr uint8_t MPU_PWR_MGMT_1   = 0x6B;
static constexpr uint8_t MPU_PWR_MGMT_2   = 0x6C;
static constexpr uint8_t MPU_SMPLRT_DIV   = 0x19;
static constexpr uint8_t MPU_CONFIG       = 0x1A;
static constexpr uint8_t MPU_GYRO_CONFIG  = 0x1B;
static constexpr uint8_t MPU_ACCEL_CONFIG = 0x1C;
static constexpr uint8_t MPU_ACCEL_CONFIG2 = 0x1D;
static constexpr uint8_t MPU_INT_PIN_CFG  = 0x37;
static constexpr uint8_t MPU_USER_CTRL    = 0x6A;
static constexpr uint8_t MPU_ACCEL_XOUT_H = 0x3B;

/* ---- Registres AK8963 (magnétomètre, accessible en bypass) ---- */
static constexpr uint8_t AK_ADDR  = 0x0C;
static constexpr uint8_t AK_WIA   = 0x00;  /* 0x48 attendu */
static constexpr uint8_t AK_ST1   = 0x02;  /* bit0 = DRDY, bit1 = DOR */
static constexpr uint8_t AK_HXL   = 0x03;  /* HXL..HZH + ST2 = 7 octets */
static constexpr uint8_t AK_ST2   = 0x09;  /* bit3 = HOFL (débordement) */
static constexpr uint8_t AK_CNTL1 = 0x0A;  /* 0x12 = 16 bits, 100 Hz continu */
static constexpr uint8_t AK_CNTL2 = 0x0B;  /* 0x01 = soft reset */
static constexpr uint8_t AK_ASAX  = 0x10;  /* corrections usine (fuse ROM) */

/* ---- Échelles (configuration appliquée) ---- */
static constexpr float ACCEL_LSB_PER_G = 4096.0f;   /* ±8 g  */
static constexpr float GYRO_LSB_PER_DPS = 32.8f;    /* ±1000 °/s */
static constexpr float MAG_UT_PER_LSB = 0.15f;      /* AK8963 16 bits */

Mpu9250 g_imu;

/* ------------------------------------------------------------------ */
/* Primitives I2C                                                      */
/* ------------------------------------------------------------------ */

bool Mpu9250::_write8(uint8_t dev, uint8_t reg, uint8_t val) {
    Wire.beginTransmission(dev);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

bool Mpu9250::_read8(uint8_t dev, uint8_t reg, uint8_t& val) {
    Wire.beginTransmission(dev);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)dev, 1) != 1) return false;
    val = Wire.read();
    return true;
}

bool Mpu9250::_readBytes(uint8_t dev, uint8_t reg, uint8_t* buf, uint8_t n) {
    Wire.beginTransmission(dev);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)dev, (int)n) != n) return false;
    for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
    return true;
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                      */
/* ------------------------------------------------------------------ */

bool Mpu9250::begin(bool autoCalibGyro) {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, IMU_I2C_FREQ_HZ);
    Wire.setTimeOut(50);
    delay(100);

    /* Détection : AD0 à GND → 0x68, AD0 à VCC → 0x69 ; les deux adresses
       sont essayées. WHO_AM_I acceptés : 0x71 (MPU9250), 0x73/0x75 (MPU9255),
       0x70 (MPU6500) et 0x68 (MPU6050) — nombreux modules « MPU9250 » sont
       en réalité des clones compatibles (fusion 6 axes, mag optionnel). */
    const uint8_t altAddr = (IMU_I2C_ADDR == 0x68) ? 0x69 : 0x68;
    const uint8_t candidates[2] = { IMU_I2C_ADDR, altAddr };
    bool responded = false;

    for (uint8_t i = 0; i < 2 && !responded; i++) {
        _addr = candidates[i];
        if (_read8(_addr, MPU_WHO_AM_I, _whoami) && _whoami != 0x00 && _whoami != 0xFF) {
            responded = true;
        }
    }

    if (!responded) {
        Serial.println("[IMU] ERREUR : aucun capteur aux adresses 0x68/0x69 (SDA=" +
                       String(PIN_I2C_SDA) + ", SCL=" + String(PIN_I2C_SCL) + ")");
        _scanBus();
        return false;
    }

    if (_whoami != 0x68 && _whoami != 0x70 && _whoami != 0x71 &&
        _whoami != 0x73 && _whoami != 0x75) {
        Serial.println("[IMU] ERREUR : WHO_AM_I inattendu (0x" + String(_whoami, HEX) +
                       " à 0x" + String(_addr, HEX) + ") — capteur non supporté");
        _scanBus();
        return false;
    }

    const char* chip = (_whoami == 0x71) ? "MPU9250" :
                       (_whoami == 0x73 || _whoami == 0x75) ? "MPU9255" :
                       (_whoami == 0x70) ? "MPU6500" : "MPU6050";
    Serial.println("[IMU] Capteur détecté : " + String(chip) +
                   " (WHO_AM_I 0x" + String(_whoami, HEX) +
                   ", adresse 0x" + String(_addr, HEX) + ")");

    /* Reset + horloge PLL */
    _write8(_addr, MPU_PWR_MGMT_1, 0x80);
    delay(100);
    _write8(_addr, MPU_PWR_MGMT_1, 0x01);
    delay(50);
    _write8(_addr, MPU_PWR_MGMT_2, 0x00);
    _write8(_addr, MPU_USER_CTRL, 0x00);       /* I2C master off → bypass */

    /* Plages et filtres : ±8 g, ±1000 °/s, DLPF 41 Hz */
    _write8(_addr, MPU_ACCEL_CONFIG, 0x10);
    _write8(_addr, MPU_ACCEL_CONFIG2, 0x03);
    _write8(_addr, MPU_GYRO_CONFIG, 0x10);
    _write8(_addr, MPU_CONFIG, 0x03);

    /* Cadence d'échantillonnage : 1 kHz / (1 + SRD) */
    const uint8_t srd = (uint8_t)(1000 / IMU_SAMPLE_HZ - 1);
    _write8(_addr, MPU_SMPLRT_DIV, srd);

    /* Magnétomètre AK8963 : visible sur le bus principal (bypass) */
    _write8(_addr, MPU_INT_PIN_CFG, 0x02);
    delay(50);
    _magOk = _initMag();
    if (!_magOk) {
        Serial.println("[IMU] AVERTISSEMENT : AK8963 introuvable (fusion 6 axes uniquement)");
    }

    delay(200); /* stabilisation */
    _ok = true;

    /* Biais gyroscopique : calibrage à froid, sinon valeur persistée */
    if (autoCalibGyro) {
        if (calibrateGyro()) {
            Serial.println("[IMU] Biais gyro calibré (" +
                           String(_gyroBias[0], 3) + ", " + String(_gyroBias[1], 3) + ", " +
                           String(_gyroBias[2], 3) + " °/s)");
        } else {
            if (loadGyroBias()) {
                Serial.println("[IMU] Mouvement détecté au démarrage — biais gyro rechargé depuis NVS");
            } else {
                Serial.println("[IMU] AVERTISSEMENT : calibrage gyro impossible, biais nul");
            }
        }
    }

    Serial.println("[IMU] Capteur initialisé (WHO_AM_I 0x" + String(_whoami, HEX) +
                   ", mag " + String(_magOk ? "OK" : "absent") + ")");
    return true;
}

void Mpu9250::_scanBus() {
    Serial.print("[IMU] Diagnostic bus I2C :");
    uint8_t found = 0;
    for (uint8_t a = 1; a < 0x7F; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.print(" 0x");
            Serial.print(a, HEX);
            found++;
        }
    }
    if (found == 0) {
        Serial.println(" aucun périphérique.");
        Serial.println("[IMU] → Vérifier le câblage SDA/SCL et l'alimentation 3V3 du module.");
    } else if (found > 8) {
        Serial.println();
        Serial.println("[IMU] → Bus bloqué (toutes les adresses répondent) : SDA ou SCL");
        Serial.println("[IMU]   court-circuitée / tirages manquants (prévoir 4,7 kΩ vers 3V3).");
    } else {
        Serial.println();
    }
}

bool Mpu9250::_initMag() {
    uint8_t wia = 0;
    if (!_read8(AK_ADDR, AK_WIA, wia) || wia != 0x48) return false;

    _write8(AK_ADDR, AK_CNTL2, 0x01);   /* soft reset */
    delay(20);

    /* Lecture des corrections usine (fuse ROM) */
    _write8(AK_ADDR, AK_CNTL1, 0x1F);   /* accès fuse ROM */
    delay(20);
    uint8_t asa[3];
    if (!_readBytes(AK_ADDR, AK_ASAX, asa, 3)) return false;
    for (int i = 0; i < 3; i++) {
        _magAdj[i] = (((float)asa[i] - 128.0f) * 0.5f / 128.0f) + 1.0f;
    }

    _write8(AK_ADDR, AK_CNTL1, 0x12);   /* 16 bits, 100 Hz continu */
    delay(20);
    return true;
}

/* ------------------------------------------------------------------ */
/* Lecture périodique                                                  */
/* ------------------------------------------------------------------ */

bool Mpu9250::readAll(float accelG[3], float gyroDps[3], float magUt[3], float* tempC) {
    if (!_ok) return false;

    uint8_t buf[14];
    if (!_readBytes(_addr, MPU_ACCEL_XOUT_H, buf, 14)) return false;

    const float ax = (int16_t)((buf[0]  << 8) | buf[1]);
    const float ay = (int16_t)((buf[2]  << 8) | buf[3]);
    const float az = (int16_t)((buf[4]  << 8) | buf[5]);
    const float t  = (int16_t)((buf[6]  << 8) | buf[7]);
    const float gx = (int16_t)((buf[8]  << 8) | buf[9]);
    const float gy = (int16_t)((buf[10] << 8) | buf[11]);
    const float gz = (int16_t)((buf[12] << 8) | buf[13]);

    accelG[0] = ax / ACCEL_LSB_PER_G;
    accelG[1] = ay / ACCEL_LSB_PER_G;
    accelG[2] = az / ACCEL_LSB_PER_G;

    gyroDps[0] = gx / GYRO_LSB_PER_DPS - _gyroBias[0];
    gyroDps[1] = gy / GYRO_LSB_PER_DPS - _gyroBias[1];
    gyroDps[2] = gz / GYRO_LSB_PER_DPS - _gyroBias[2];

    _tempC = t / 333.87f + 21.0f;
    if (tempC) *tempC = _tempC;

    /* Magnétomètre : lu uniquement si une mesure est prête (DRDY) */
    if (_magOk) {
        uint8_t st1 = 0;
        if (_read8(AK_ADDR, AK_ST1, st1) && (st1 & 0x01)) {
            uint8_t m[7];
            if (_readBytes(AK_ADDR, AK_HXL, m, 7) && !(m[6] & 0x08)) { /* ST2 : HOFL=0 */
                magUt[0] = (int16_t)((m[0] << 8) | m[1]) * MAG_UT_PER_LSB * _magAdj[0];
                magUt[1] = (int16_t)((m[2] << 8) | m[3]) * MAG_UT_PER_LSB * _magAdj[1];
                magUt[2] = (int16_t)((m[4] << 8) | m[5]) * MAG_UT_PER_LSB * _magAdj[2];
            }
        }
    } else {
        magUt[0] = magUt[1] = magUt[2] = 0.0f;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Calibrage du biais gyroscopique (NVS vérifiée par relecture)        */
/* ------------------------------------------------------------------ */

bool Mpu9250::calibrateGyro() {
    if (!_ok) return false;

    const uint16_t n = IMU_GYRO_CALIB_SAMPLES;
    float sum[3] = {0.0f, 0.0f, 0.0f};
    float sumSq[3] = {0.0f, 0.0f, 0.0f};
    float accelG[3], gyroDps[3], magUt[3];

    Serial.println("[IMU] Calibrage gyro : gardez l'appareil immobile…");

    for (uint16_t i = 0; i < n; i++) {
        if (!readAll(accelG, gyroDps, magUt)) return false;
        for (int a = 0; a < 3; a++) {
            /* Biais courant non retiré : lecture brute renvoyée avant calibrage */
            sum[a] += gyroDps[a];
            sumSq[a] += gyroDps[a] * gyroDps[a];
        }
        delay(5);
    }

    /* Variance (échantillons bruts, biais encore non appliqué) */
    for (int a = 0; a < 3; a++) {
        const float mean = sum[a] / n;
        const float var = (sumSq[a] / n) - mean * mean;
        const float stdev = sqrtf(var > 0.0f ? var : 0.0f);
        if (stdev > IMU_GYRO_STILL_STDEV_DPS) {
            Serial.println("[IMU] ERREUR : mouvement détecté pendant le calibrage");
            return false;
        }
        _gyroBias[a] += mean;   /* cumul : gère un biais préchargé depuis NVS */
    }

    /* Persistance NVS avec relecture de confirmation (convention du socle) */
    Preferences prefs;
    if (!prefs.begin("imu", false)) return false;
    prefs.putFloat("gxb", _gyroBias[0]);
    prefs.putFloat("gyb", _gyroBias[1]);
    prefs.putFloat("gzb", _gyroBias[2]);
    const bool ok = (prefs.getFloat("gxb", 999.0f) == _gyroBias[0]) &&
                    (prefs.getFloat("gyb", 999.0f) == _gyroBias[1]) &&
                    (prefs.getFloat("gzb", 999.0f) == _gyroBias[2]);
    prefs.end();
    return ok;
}

bool Mpu9250::loadGyroBias() {
    Preferences prefs;
    if (!prefs.begin("imu", true)) return false;
    const bool has = prefs.isKey("gxb");
    if (has) {
        _gyroBias[0] = prefs.getFloat("gxb", 0.0f);
        _gyroBias[1] = prefs.getFloat("gyb", 0.0f);
        _gyroBias[2] = prefs.getFloat("gzb", 0.0f);
    }
    prefs.end();
    return has;
}
