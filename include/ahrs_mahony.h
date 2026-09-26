#ifndef AHRS_MAHONY_H
#define AHRS_MAHONY_H

/**
 * @brief Filtre de fusion inertielle Mahony 6 axes (gyroscope + accéléromètre).
 *
 * Implémentation directe de l'algorithme de référence (S. Madgwick, x-io
 * Technologies) : la pesanteur estimée à partir du quaternion est comparée à
 * la pesanteur mesurée par l'accéléromètre ; l'erreur (produit vectoriel)
 * corrige le gyroscope en proportionnel + intégral, puis le quaternion est
 * intégré.
 *
 * Le lacet n'est pas contraint par l'accéléromètre : il s'agit d'une
 * intégration gyroscopique (dérive lente, ~1°/min après calibrage du biais),
 * corrigée par la routine de « tare » (recentrage) de la classe HeadTracker.
 *
 * Conventions (repère monde : Z vers le haut — capteur posé à plat,
 * X vers l'avant, Y vers la gauche) :
 *  - quaternion interne q : rotation capteur → monde (v_monde = q ⊗ v_cap ⊗ q*)
 *  - Tangage (pitch) > 0 : nez vers le HAUT
 *  - Roulis  (roll)  > 0 : tête penchée à DROITE
 *  - Lacet   (yaw)   > 0 : tête tournée vers la DROITE
 */
class MahonyAhrs {
public:
    /**
     * @brief Initialise le filtre.
     * @param twoKp 2×Kp : gain proportionnel (réactivité de convergence)
     * @param twoKi 2×Ki : gain intégral (compensation du biais gyro résiduel)
     */
    void begin(float twoKp, float twoKi) {
        _twoKp = twoKp;
        _twoKi = twoKi;
        _q0 = 1.0f; _q1 = 0.0f; _q2 = 0.0f; _q3 = 0.0f;
        _ifb0 = 0.0f; _ifb1 = 0.0f; _ifb2 = 0.0f;
    }

    /**
     * @brief Met à jour la fusion pour un échantillon.
     * @param gx,gy,gz Vitesse angulaire en rad/s (biais gyro déjà retiré)
     * @param ax,ay,az Accélération en g (brute, pesanteur comprise)
     * @param dt        Période réelle depuis le précédent appel (s)
     */
    void update(float gx, float gy, float gz,
                float ax, float ay, float az,
                float dt) {
        float q0 = _q0, q1 = _q1, q2 = _q2, q3 = _q3;

        /* Correction accélérométrique (si mesure exploitable) */
        if (ax != 0.0f || ay != 0.0f || az != 0.0f) {
            float recipNorm = 1.0f / sqrtf(ax * ax + ay * ay + az * az);
            ax *= recipNorm; ay *= recipNorm; az *= recipNorm;

            /* Direction estimée de la pesanteur dans le repère capteur */
            float halfvx = q1 * q3 - q0 * q2;
            float halfvy = q0 * q1 + q2 * q3;
            float halfvz = q0 * q0 - 0.5f + q3 * q3;

            /* Erreur = produit vectoriel (mesuré × estimé) */
            float halfex = ay * halfvz - az * halfvy;
            float halfey = az * halfvx - ax * halfvz;
            float halfez = ax * halfvy - ay * halfvx;

            /* Terme intégral (compensation du biais résiduel) */
            if (_twoKi > 0.0f) {
                _ifb0 += _twoKi * halfex * dt;
                _ifb1 += _twoKi * halfey * dt;
                _ifb2 += _twoKi * halfez * dt;
                gx += _ifb0; gy += _ifb1; gz += _ifb2;
            }

            /* Terme proportionnel */
            gx += _twoKp * halfex;
            gy += _twoKp * halfey;
            gz += _twoKp * halfez;
        }

        /* Intégration du quaternion (q += 0.5 ⊗ q ⊗ ω · dt) */
        float qDot0 = 0.5f * (-q1 * gx - q2 * gy - q3 * gz);
        float qDot1 = 0.5f * ( q0 * gx + q2 * gz - q3 * gy);
        float qDot2 = 0.5f * ( q0 * gy - q1 * gz + q3 * gx);
        float qDot3 = 0.5f * ( q0 * gz + q1 * gy - q2 * gx);
        q0 += qDot0 * dt; q1 += qDot1 * dt;
        q2 += qDot2 * dt; q3 += qDot3 * dt;

        /* Normalisation */
        float norm = 1.0f / sqrtf(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
        _q0 = q0 * norm; _q1 = q1 * norm; _q2 = q2 * norm; _q3 = q3 * norm;
    }

    /**
     * @brief Extrait les angles d'Euler (radians), conventions projet.
     * @param pitchUp   Tangage, positif nez vers le haut
     * @param rollRight Roulis, positif tête penchée à droite
     * @param yawRight  Lacet, positif tête tournée vers la droite
     */
    void getEulerRad(float& pitchUp, float& rollRight, float& yawRight) const {
        float sinp = 2.0f * (_q1 * _q3 - _q0 * _q2);
        if (sinp >  1.0f) sinp =  1.0f;
        if (sinp < -1.0f) sinp = -1.0f;
        pitchUp   = asinf(sinp);
        rollRight = atan2f(2.0f * (_q2 * _q3 + _q0 * _q1),
                           1.0f - 2.0f * (_q1 * _q1 + _q2 * _q2));
        yawRight  = -atan2f(2.0f * (_q1 * _q2 + _q0 * _q3),
                            1.0f - 2.0f * (_q2 * _q2 + _q3 * _q3));
    }

    /**
     * @brief Direction de la pesanteur dans le repère capteur (en g).
     *
     * À l'équilibre, l'accéléromètre mesure exactement ce vecteur :
     * accélération linéaire = mesure − getGravityG().
     */
    void getGravityG(float g[3]) const {
        g[0] = 2.0f * (_q1 * _q3 - _q0 * _q2);
        g[1] = 2.0f * (_q0 * _q1 + _q2 * _q3);
        g[2] = 1.0f - 2.0f * (_q1 * _q1 + _q2 * _q2);
    }

private:
    float _twoKp = 2.0f;
    float _twoKi = 0.1f;
    float _q0 = 1.0f, _q1 = 0.0f, _q2 = 0.0f, _q3 = 0.0f;
    float _ifb0 = 0.0f, _ifb1 = 0.0f, _ifb2 = 0.0f; /* retour intégral */
};

#endif /* AHRS_MAHONY_H */
