#ifndef BOBBLAR_CONFIG_H
#define BOBBLAR_CONFIG_H

#include <Arduino.h>

/* =========================================================================
 * SECTION 1 : IDENTITÉ DU PROJET
 * ========================================================================= */

/** @brief Nom du projet — également le nom Bluetooth de la manette (appairage) */
constexpr const char* PROJECT_NAME = "BOB BL AR";

/** @brief Auteur affiché dans le pied de page de l'interface web */
constexpr const char* PROJECT_AUTHOR = "BOB";

/** @brief Licence affichée dans le pied de page de l'interface web */
constexpr const char* PROJECT_LICENSE = "Licence MIT";

/* =========================================================================
 * SECTION 2 : CONFIGURATION RÉSEAU WI-FI ET POINT D'ACCÈS
 * ========================================================================= */

/** @brief SSID du point d'accès Wi-Fi autonome */
constexpr const char* AP_SSID = "BOB BL AR";

/** @brief Mot de passe du point d'accès (min. 8 caractères WPA2) */
constexpr const char* AP_PASSWORD = "bobblar2026";

/** @brief Adresse IP statique du point d'accès */
constexpr const char* AP_IP = "192.168.4.1";

/** @brief Canal Wi-Fi du point d'accès */
constexpr uint8_t AP_CHANNEL = 6;

/** @brief Nombre maximum de clients Wi-Fi simultanés */
constexpr uint8_t AP_MAX_CLIENTS = 4;

/** @brief Code pays Wi-Fi — « FR » autorise les canaux 1-13. Le réglage par
 *         défaut du pilote (« monde ») ne scanne que 1-11 : une box française
 *         sur canal 12/13 devient invisible au scan. */
constexpr const char* WIFI_COUNTRY_CODE = "FR";

/** @brief Temps de séjour par canal du scan actif (ms) — 300 ms : rattrape
 *         les réseaux discrets malgré les retours périodiques au canal AP. */
constexpr uint32_t WIFI_SCAN_DWELL_MS = 300;

/** @brief Budget total d'un scan lancé depuis l'UI : démarrage + exécution. */
constexpr uint32_t WIFI_SCAN_BUDGET_MS = 8000;

/** @brief Stack et priorité de la tâche Web (sur C6 monocœur : cœur 0) */
constexpr uint32_t STACK_SIZE_WEB = 8192;
constexpr uint8_t PRIORITY_WEB = 1;

/** @brief Période de diffusion de la télémétrie WebSocket (ms) */
constexpr uint16_t TELEMETRY_PERIOD_MS = 100; /* 10 Hz */

/* =========================================================================
 * SECTION 3 : MANETTE BLE HID (NimBLE)
 * ========================================================================= */

/** @brief Nom Bluetooth visible par macOS / Windows lors de l'appairage */
constexpr const char* BLE_DEVICE_NAME = "BOB BL AR";

/** @brief Fabricant annoncé dans le service Device Information (0x2A29) */
constexpr const char* BLE_MANUFACTURER = "BOB";

/** @brief VID/PNP « vendor-assigned » (0x02) — pid.codes, usage communautaire */
constexpr uint16_t BLE_VID = 0x1209;
constexpr uint16_t BLE_PID = 0xB0B1;
constexpr uint16_t BLE_VERSION = 0x0100;

/** @brief Apparence GAP : HID Gamepad (0x03C4) — icône manette côté hôte */
constexpr uint16_t BLE_APPEARANCE_GAMEPAD = 0x03C4;

/** @brief Niveau de batterie annoncé (service 0x180F) — pas de mesure réelle */
constexpr uint8_t BLE_BATTERY_LEVEL = 100;

/** @brief Identifiant du rapport HID envoyé en notification */
constexpr uint8_t GAMEPAD_REPORT_ID = 0x01;

/** @brief Intervalle minimum entre deux notifications HID (ms) — ~66 Hz max */
constexpr uint16_t BLE_SEND_MIN_INTERVAL_MS = 15;

/** @brief Anti-rebond des ordres de rumble reçus sur l'Output Report (ms) —
 *         un effet continu relancé périodiquement par l'hôte ne déclenche
 *         qu'une seule tare par fenêtre (protège la NVS et la trace série). */
constexpr uint16_t BLE_RUMBLE_DEBOUNCE_MS = 250;

/* =========================================================================
 * SECTION 4 : CAPTEUR INERTIEL MPU9250 (I2C)
 * ========================================================================= */

/** @brief Broche SDA — câblage du projet (GPIO4 = J1 pin 3 du DevKitC-1) */
constexpr uint8_t PIN_I2C_SDA = 4;

/** @brief Broche SCL — câblage du projet (GPIO5 = J1 pin 4 du DevKitC-1) */
constexpr uint8_t PIN_I2C_SCL = 5;

/** @brief Adresse I2C du MPU9250 (0x69 si AD0 tiré à VCC — l'alternative
 *         est essayée automatiquement au démarrage) */
constexpr uint8_t IMU_I2C_ADDR = 0x68;

/** @brief Fréquence du bus I2C (Hz) — 100 kHz : tolère les tirages internes
 *         faibles (~45 kΩ) et les fils Dupont (14 octets × 100 Hz ≈ 13 % du bus).
 *         Revenir à 400 kHz uniquement avec des tirages externes 4,7 kΩ. */
constexpr uint32_t IMU_I2C_FREQ_HZ = 100000;

/** @brief Fréquence d'échantillonnage capteur + fusion (Hz) */
constexpr uint16_t IMU_SAMPLE_HZ = 100;

/** @brief Nombre d'échantillons du calibrage gyroscopique (~2 s à 5 ms) */
constexpr uint16_t IMU_GYRO_CALIB_SAMPLES = 200;

/** @brief Écart-type maximum (°/s) considéré comme « capteur immobile » au calibrage */
constexpr float IMU_GYRO_STILL_STDEV_DPS = 0.5f;

/** @brief Gain proportionnel du filtre Mahony (2×Kp) */
constexpr float MAHONY_TWO_KP = 2.0f;

/** @brief Gain intégral du filtre Mahony (2×Ki) */
constexpr float MAHONY_TWO_KI = 0.1f;

/** @brief Coefficient du filtre passe-bas de l'accélération linéaire (jerk) */
constexpr float LINACC_EMA_ALPHA = 0.7f;

/** @brief Accélération normale de la pesanteur (m/s²) */
constexpr float G_MS2 = 9.80665f;

/* =========================================================================
 * SECTION 5 : MAPPING MANETTE (défauts — modifiables via l'interface, NVS)
 * ========================================================================= */

/** @brief Zone morte par défaut du tangage (°) */
constexpr float GP_DEFAULT_DEADZONE_PITCH = 5.0f;

/** @brief Zone morte par défaut du roulis (°) */
constexpr float GP_DEFAULT_DEADZONE_ROLL = 5.0f;

/** @brief Zone morte par défaut du lacet (°) */
constexpr float GP_DEFAULT_DEADZONE_YAW = 8.0f;

/** @brief Sensibilité : angle (°) provoquant la pleine déviation (±127) */
constexpr float GP_DEFAULT_FULL_DEFLECT = 35.0f;

/** @brief Seuil d'accélération linéaire déclenchant le Bouton 1 (m/s²) */
constexpr float GP_DEFAULT_JERK_THRESHOLD = 15.0f;

/** @brief Temps de recharge du Bouton 1 après un jerk (ms) */
constexpr uint16_t GP_DEFAULT_JERK_COOLDOWN = 500;

/** @brief Bornes de validation des réglages (REST → 400 si hors bornes) */
constexpr float GP_DEADZONE_MIN_DEG = 0.0f;
constexpr float GP_DEADZONE_MAX_DEG = 45.0f;
constexpr float GP_FULL_DEFLECT_MIN_DEG = 10.0f;
constexpr float GP_FULL_DEFLECT_MAX_DEG = 90.0f;
constexpr float GP_JERK_TH_MIN_MS2 = 3.0f;
constexpr float GP_JERK_TH_MAX_MS2 = 60.0f;
constexpr uint16_t GP_COOLDOWN_MIN_MS = 100;
constexpr uint16_t GP_COOLDOWN_MAX_MS = 3000;

/** @brief Échelle d'affichage de la barre d'accélération (m/s²) — côté web */
constexpr float LINACC_DISPLAY_MAX_MS2 = 60.0f;

/* =========================================================================
 * SECTION 6 : TÂCHES FREERTOS
 * ========================================================================= */

/**
 * @brief Stack et priorité de la tâche manette (fusion + mapping + HID).
 *
 * NOTE C6 : l'ESP32-C6 est MONOCŒUR (RISC-V). Le socle d'origine épingle la
 * logique temps réel sur le cœur 1 ; ici la tâche est créée via xTaskCreate
 * (cœur unique) avec une priorité supérieure à la tâche Web pour garantir
 * la cadence 100 Hz. Sur une cible bicœur (S3), le code re-épingle sur le
 * cœur 1 automatiquement (voir head_tracker.cpp).
 *
 * 8192 o : la tâche exécute aussi la tare demandée par l'Output Report HID
 * (rumble) — écriture NVS complète + relecture, chemin plus profond que la
 * boucle de mesure seule (cf. docs/TARE_FORCE_FEEDBACK.md § 5.2).
 */
constexpr uint32_t STACK_SIZE_GAMEPAD = 8192;
constexpr uint8_t PRIORITY_GAMEPAD = 4;

/** @brief Période de la tâche manette (ms) — 10 ms = 100 Hz */
constexpr uint16_t GAMEPAD_PERIOD_MS = 10;

#endif /* BOBBLAR_CONFIG_H */
