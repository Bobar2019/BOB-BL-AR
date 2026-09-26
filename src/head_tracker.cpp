#include "head_tracker.h"
#include <Preferences.h>

HeadTracker g_headTracker;

/* ------------------------------------------------------------------ */
/* Amorçage                                                            */
/* ------------------------------------------------------------------ */

bool HeadTracker::begin() {
    _mutex = xSemaphoreCreateMutex();

    _loadSettings();

    _ahrs.begin(MAHONY_TWO_KP, MAHONY_TWO_KI);

    _imuOk = g_imu.begin(/*autoCalibGyro*/ true);
    if (!_imuOk) {
        Serial.println("[TRACK] ERREUR : MPU9250 indisponible — la manette restera inactive");
    }
    return _imuOk;
}

void HeadTracker::startTask() {
#if CONFIG_FREERTOS_UNICORE
    /* ESP32-C6 : monocœur — priorité > tâche web pour la cadence 100 Hz */
    xTaskCreate(_taskEntry, "Gamepad", STACK_SIZE_GAMEPAD, this, PRIORITY_GAMEPAD, nullptr);
#else
    /* Cible bicœur (ESP32-S3…) : logique temps réel épinglée sur le cœur 1 */
    xTaskCreatePinnedToCore(_taskEntry, "Gamepad", STACK_SIZE_GAMEPAD, this,
                            PRIORITY_GAMEPAD, nullptr, 1);
#endif
    Serial.println("[TRACK] Tâche manette démarrée (100 Hz)");
}

/* ------------------------------------------------------------------ */
/* Réglages NVS (namespace « gamepad », écriture vérifiée par relecture) */
/* ------------------------------------------------------------------ */

void HeadTracker::_loadSettings() {
    Preferences prefs;
    if (!prefs.begin("gamepad", true)) {
        Serial.println("[TRACK] Réglages par défaut (NVS vierge)");
        return;
    }
    _settings.deadzonePitchDeg = prefs.getFloat("dzP", GP_DEFAULT_DEADZONE_PITCH);
    _settings.deadzoneRollDeg  = prefs.getFloat("dzR", GP_DEFAULT_DEADZONE_ROLL);
    _settings.deadzoneYawDeg   = prefs.getFloat("dzY", GP_DEFAULT_DEADZONE_YAW);
    /* Migration : l'ancienne clé unique « full » amorce les trois axes tant
       que les nouvelles clés n'ont pas été écrites (valeur > 0 forcément). */
    const float legacyFull = prefs.getFloat("full", -1.0f);
    const float fdDefault  = (legacyFull > 0.0f) ? legacyFull : (float)GP_DEFAULT_FULL_DEFLECT;
    _settings.fullDeflectPitchDeg = prefs.getFloat("fdP", fdDefault);
    _settings.fullDeflectRollDeg  = prefs.getFloat("fdR", fdDefault);
    _settings.fullDeflectYawDeg   = prefs.getFloat("fdY", fdDefault);
    _settings.jerkThresholdMs2 = prefs.getFloat("jerk", GP_DEFAULT_JERK_THRESHOLD);
    _settings.jerkCooldownMs   = prefs.getUShort("cool", GP_DEFAULT_JERK_COOLDOWN);
    const uint8_t inv = prefs.getUChar("inv", 0);
    _settings.invertX = (inv & 0x01) != 0;
    _settings.invertY = (inv & 0x02) != 0;
    _settings.invertZ = (inv & 0x04) != 0;
    _tarePitch = prefs.getFloat("tarP", 0.0f);
    _tareRoll  = prefs.getFloat("tarR", 0.0f);
    _tareYaw   = prefs.getFloat("tarY", 0.0f);
    prefs.end();
    Serial.println("[TRACK] Réglages chargés (dz=" + String(_settings.deadzonePitchDeg, 1) +
                   "/" + String(_settings.deadzoneRollDeg, 1) + "/" + String(_settings.deadzoneYawDeg, 1) +
                   "°, pleine déviation " + String(_settings.fullDeflectPitchDeg, 0) + "/" +
                   String(_settings.fullDeflectRollDeg, 0) + "/" + String(_settings.fullDeflectYawDeg, 0) +
                   "°, jerk " + String(_settings.jerkThresholdMs2, 1) + " m/s²)");
}

bool HeadTracker::_saveSettings() {
    Preferences prefs;
    if (!prefs.begin("gamepad", false)) return false;

    prefs.putFloat("dzP", _settings.deadzonePitchDeg);
    prefs.putFloat("dzR", _settings.deadzoneRollDeg);
    prefs.putFloat("dzY", _settings.deadzoneYawDeg);
    prefs.putFloat("fdP", _settings.fullDeflectPitchDeg);
    prefs.putFloat("fdR", _settings.fullDeflectRollDeg);
    prefs.putFloat("fdY", _settings.fullDeflectYawDeg);
    prefs.remove("full");   /* clé unique remplacée par fdP/fdR/fdY */
    prefs.putFloat("jerk", _settings.jerkThresholdMs2);
    prefs.putUShort("cool", _settings.jerkCooldownMs);
    const uint8_t inv = (_settings.invertX ? 0x01 : 0) |
                        (_settings.invertY ? 0x02 : 0) |
                        (_settings.invertZ ? 0x04 : 0);
    prefs.putUChar("inv", inv);
    prefs.putFloat("tarP", _tarePitch);
    prefs.putFloat("tarR", _tareRoll);
    prefs.putFloat("tarY", _tareYaw);

    /* Relecture de confirmation (convention du socle) */
    const bool ok = prefs.getFloat("dzP", -1.0f) == _settings.deadzonePitchDeg &&
                    prefs.getFloat("fdP", -1.0f) == _settings.fullDeflectPitchDeg &&
                    prefs.getFloat("fdR", -1.0f) == _settings.fullDeflectRollDeg &&
                    prefs.getFloat("fdY", -1.0f) == _settings.fullDeflectYawDeg &&
                    prefs.getFloat("jerk", -1.0f) == _settings.jerkThresholdMs2 &&
                    prefs.getUShort("cool", 0) == _settings.jerkCooldownMs &&
                    prefs.getUChar("inv", 0xFF) == inv;
    prefs.end();
    return ok;
}

bool HeadTracker::applySettings(const GamepadSettings& s) {
    /* Bornage systématique (défense en profondeur face à l'API REST) */
    GamepadSettings b = s;
    auto clampf = [](float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };
    b.deadzonePitchDeg = clampf(b.deadzonePitchDeg, GP_DEADZONE_MIN_DEG, GP_DEADZONE_MAX_DEG);
    b.deadzoneRollDeg  = clampf(b.deadzoneRollDeg,  GP_DEADZONE_MIN_DEG, GP_DEADZONE_MAX_DEG);
    b.deadzoneYawDeg   = clampf(b.deadzoneYawDeg,   GP_DEADZONE_MIN_DEG, GP_DEADZONE_MAX_DEG);
    b.fullDeflectPitchDeg = clampf(b.fullDeflectPitchDeg, GP_FULL_DEFLECT_MIN_DEG, GP_FULL_DEFLECT_MAX_DEG);
    b.fullDeflectRollDeg  = clampf(b.fullDeflectRollDeg,  GP_FULL_DEFLECT_MIN_DEG, GP_FULL_DEFLECT_MAX_DEG);
    b.fullDeflectYawDeg   = clampf(b.fullDeflectYawDeg,   GP_FULL_DEFLECT_MIN_DEG, GP_FULL_DEFLECT_MAX_DEG);
    b.jerkThresholdMs2 = clampf(b.jerkThresholdMs2, GP_JERK_TH_MIN_MS2, GP_JERK_TH_MAX_MS2);
    b.jerkCooldownMs   = (uint16_t)constrain((long)b.jerkCooldownMs, GP_COOLDOWN_MIN_MS, GP_COOLDOWN_MAX_MS);

    _settings = b;
    const bool ok = _saveSettings();
    Serial.println("[TRACK] Réglages appliqués : dz " + String(b.deadzonePitchDeg, 1) + "/" +
                   String(b.deadzoneRollDeg, 1) + "/" + String(b.deadzoneYawDeg, 1) +
                   "° · pleine déviation " + String(b.fullDeflectPitchDeg, 0) + "/" +
                   String(b.fullDeflectRollDeg, 0) + "/" + String(b.fullDeflectYawDeg, 0) +
                   "° · jerk " + String(b.jerkThresholdMs2, 1) + " m/s² · recharge " +
                   String(b.jerkCooldownMs) + " ms");
    return ok;
}

/* ------------------------------------------------------------------ */
/* Tare (point 0 de la tête)                                           */
/* ------------------------------------------------------------------ */

bool HeadTracker::tare() {
    /* Les angles bruts sont écrits par la tâche 100 Hz ; position actuelle
       = nouveau neutre pour les trois axes. */
    _tarePitch = _rawPitchDeg;
    _tareRoll  = _rawRollDeg;
    _tareYaw   = _rawYawDeg;

    const bool ok = _saveSettings();
    Serial.println("[TRACK] Tare effectué (offsets " + String(_tarePitch, 1) + ", " +
                   String(_tareRoll, 1) + ", " + String(_tareYaw, 1) + "°)");
    return ok;
}

TrackerTelemetry HeadTracker::telemetry() {
    TrackerTelemetry copy;
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        copy = _tele;
        xSemaphoreGive(_mutex);
    }
    return copy;
}

/* ------------------------------------------------------------------ */
/* Mapping angle → axe HID                                             */
/*                                                                     */
/* Cahier des charges :                                                */
/*   tête en avant (pitch < 0)  → J1 Y− (avant)                        */
/*   tête en arrière (pitch > 0) → J1 Y+ (arrière)                     */
/*   tête à droite (roll > 0)    → J1 X+                               */
/*   tête à gauche (roll < 0)    → J1 X−                               */
/*   tête tournée à droite (yaw > 0) → J2 Z+                           */
/*   tête tournée à gauche (yaw < 0) → J2 Z−                           */
/* ------------------------------------------------------------------ */

float HeadTracker::_mapAngle(float angleDeg, float deadzoneDeg, float fullDeflectDeg) const {
    const float dz = deadzoneDeg;
    float span = fullDeflectDeg - dz;
    if (span < 1.0f) span = 1.0f;

    const float a = fabsf(angleDeg);
    if (a <= dz) return 0.0f;                    /* zone morte : anti-jitter */

    float v = (a - dz) / span;                   /* 0..1 au-delà de la zone morte */
    if (v > 1.0f) v = 1.0f;
    return (angleDeg < 0.0f ? -v : v) * 127.0f;
}

/* ------------------------------------------------------------------ */
/* Conversion d'axe : ±127 (échelle interne du socle) → u16 « Xbox »,   */
/* 0x8000 = centré, pleine butée à ±32767 autour du centre.            */
/* ------------------------------------------------------------------ */

static uint16_t axisToXboxU16(float v) {
    const float c = constrain(v, -127.0f, 127.0f);
    return (uint16_t)(32768 + (int32_t)lroundf(c * (32767.0f / 127.0f)));
}

/* ------------------------------------------------------------------ */
/* Tâche temps réel 100 Hz                                             */
/* ------------------------------------------------------------------ */

void HeadTracker::_taskEntry(void* param) {
    static_cast<HeadTracker*>(param)->_taskLoop();
}

void HeadTracker::_taskLoop() {
    TickType_t lastWake = xTaskGetTickCount();
    uint32_t lastMicros = micros();

    float accelG[3] = {0, 0, 0};
    float gyroDps[3] = {0, 0, 0};
    float magUt[3] = {0, 0, 0};

    HidGamepadReport lastSent = {};   /* zéros ≠ neutre (0x8000) : 1er rapport garanti */
    uint32_t lastSendMs = 0;
    uint32_t connectedAtMs = 0;       /* début de connexion : période de grâce d'émission */
    bool bleConnectedLatch = false;

    for (;;) {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(GAMEPAD_PERIOD_MS));

        /* 0. Tare demandée depuis le callback BLE (ordre de rumble, cf.
              docs/TARE_FORCE_FEEDBACK.md) : exécutée ICI, dans le contexte
              de la tâche manette — jamais dans le callback NimBLE, où une
              écriture NVS est proscrite (pile limitée). Le rapport d'entrée
              suivant repart donc des angles recentrés (≤ 10 ms + intervalle
              de notification BLE). */
        if (_tareRequested) {
            _tareRequested = false;
            tare();
        }

        if (!_imuOk) {
            /* Capteur absent : télémétrie d'état uniquement */
            if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
                _tele.imuOk = false;
                _tele.bleConnected = g_bleGamepad.isConnected();
                xSemaphoreGive(_mutex);
            }
            continue;
        }

        /* 1. Mesure */
        const uint32_t nowMicros = micros();
        float dt = (nowMicros - lastMicros) * 1e-6f;
        lastMicros = nowMicros;
        if (dt <= 0.0f || dt > 0.05f) dt = 0.01f;

        if (!g_imu.readAll(accelG, gyroDps, magUt)) {
            if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
                _tele.i2cErrors++;
                xSemaphoreGive(_mutex);
            }
            continue;
        }

        /* 2. Fusion Mahony (gyro en rad/s, accéléro en g) */
        _ahrs.update(gyroDps[0] * DEG_TO_RAD, gyroDps[1] * DEG_TO_RAD, gyroDps[2] * DEG_TO_RAD,
                     accelG[0], accelG[1], accelG[2], dt);

        float pitchRad, rollRad, yawRad;
        _ahrs.getEulerRad(pitchRad, rollRad, yawRad);
        const float pitchRaw = pitchRad * RAD_TO_DEG;
        const float rollRaw  = rollRad  * RAD_TO_DEG;
        const float yawRaw   = yawRad   * RAD_TO_DEG;

        /* Publication des angles bruts (tare depuis la tâche web) */
        _rawPitchDeg = pitchRaw;
        _rawRollDeg  = rollRaw;
        _rawYawDeg   = yawRaw;

        /* 3. Accélération linéaire = mesure − pesanteur estimée */
        float g[3];
        _ahrs.getGravityG(g);
        const float lx = (accelG[0] - g[0]) * G_MS2;
        const float ly = (accelG[1] - g[1]) * G_MS2;
        const float lz = (accelG[2] - g[2]) * G_MS2;
        const float linMag = sqrtf(lx * lx + ly * ly + lz * lz);
        _linAccFilt = LINACC_EMA_ALPHA * _linAccFilt + (1.0f - LINACC_EMA_ALPHA) * linMag;

        /* Détection de jerk (Bouton 1) : pic + hystérésis + recharge */
        const uint32_t nowMs = millis();
        if (!_btnActive &&
            _linAccFilt > _settings.jerkThresholdMs2 &&
            (nowMs - _lastJerkMs) >= _settings.jerkCooldownMs) {
            _btnActive = true;
            _lastJerkMs = nowMs;
        } else if (_btnActive && _linAccFilt < _settings.jerkThresholdMs2 * 0.6f) {
            _btnActive = false;
        }

        /* 4. Mapping (angles tarés + zones mortes + sensibilité + inversions) */
        const float pitch = pitchRaw - _tarePitch;
        const float roll  = rollRaw  - _tareRoll;
        const float yaw   = yawRaw   - _tareYaw;

        float j1x = _mapAngle(roll,  _settings.deadzoneRollDeg,  _settings.fullDeflectRollDeg);
        float j1y = _mapAngle(pitch, _settings.deadzonePitchDeg, _settings.fullDeflectPitchDeg);
        float j2z = _mapAngle(yaw,   _settings.deadzoneYawDeg,   _settings.fullDeflectYawDeg);
        if (_settings.invertX) j1x = -j1x;
        if (_settings.invertY) j1y = -j1y;
        if (_settings.invertZ) j2z = -j2z;

        HidGamepadReport rep;
        rep.x = axisToXboxU16(j1x);      /* roulis  → stick gauche X */
        rep.y = axisToXboxU16(j1y);      /* tangage → stick gauche Y */
        rep.z = axisToXboxU16(j2z);      /* lacet   → stick droit X  */
        rep.rz = XBOX_AXIS_CENTER;       /* réservé : centré (0 = butée basse !) */
        rep.brake = 0;                   /* gâchettes relâchées */
        rep.accelerator = 0;
        rep.hat = 0;                     /* D-pad neutre */
        rep.buttons = _btnActive ? XBOX_BTN_A : 0x0000;   /* jerk → bouton A */
        rep.share = 0;

        /* 5. Rapport BLE HID : au changement, ou en rafale périodique
              pendant les 2 s qui suivent la connexion (l'abonnement CCCD
              de l'hôte peut être postérieur au premier rapport ; sans
              activité, certains hôtes ne matérialisent pas la manette
              dans navigator.getGamepads()), débit limité. */
        const bool bleConnected = g_bleGamepad.isConnected();
        if (bleConnected != bleConnectedLatch) {
            bleConnectedLatch = bleConnected;
            if (bleConnected) connectedAtMs = nowMs;
            Serial.println(bleConnected ? "[TRACK] Diffusion HID active" : "[TRACK] Diffusion HID suspendue");
        }
        const bool grace = bleConnected && (nowMs - connectedAtMs) < 2000;
        if (bleConnected &&
            (grace || (memcmp(&rep, &lastSent, sizeof(rep)) != 0)) &&
            (nowMs - lastSendMs) >= (grace ? 250 : BLE_SEND_MIN_INTERVAL_MS)) {
            g_bleGamepad.sendReport(rep);
            lastSent = rep;
            lastSendMs = nowMs;
        }

        /* 6. Instantané télémétrie (consommé par le WebSocket à 10 Hz) */
        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
            _tele.yawDeg = yaw;
            _tele.pitchDeg = pitch;
            _tele.rollDeg = roll;
            _tele.linAccMs2 = _linAccFilt;
            _tele.j1x = j1x;
            _tele.j1y = j1y;
            _tele.j2z = j2z;
            _tele.btn1 = _btnActive;
            _tele.imuOk = true;
            _tele.bleConnected = bleConnected;
            xSemaphoreGive(_mutex);
        }
    }
}
