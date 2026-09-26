#include <Arduino.h>
#include <LittleFS.h>
#include "config.h"
#include "git_version.h"
#include "imu_mpu9250.h"
#include "ble_gamepad.h"
#include "head_tracker.h"
#include "web_server.h"

/* ------------------------------------------------------------------ */
/* Canal de commande hôte → tare (Output Report HID « dual-rumble »)    */
/* ------------------------------------------------------------------ */

/**
 * @brief Ordre de rumble reçu de l'hôte (tâche NimBLE — rester léger).
 *
 * Toute magnitude non nulle (Web Gamepad API / WebHID / script hidapi)
 * demande le recentrage du point 0 de la tête sans passer par le Wi-Fi.
 * La tare elle-même est exécutée par la tâche manette 100 Hz.
 */
static void _onRumbleOrder(uint8_t strongMagnitude, uint8_t weakMagnitude,
                            uint16_t durationMs) {
    (void)strongMagnitude;
    (void)weakMagnitude;
    (void)durationMs;
    g_headTracker.requestTare();
}

void setup() {
    Serial.begin(115200);
    delay(1000);   /* USB Serial/JTAG : laisse le temps au port de s'ouvrir */

    Serial.println("\n==============================================");
    Serial.println("  BOB BL AR " + String(GIT_VERSION) + " - Firmware");
    Serial.println("  ESP32-C6 · PlatformIO / FreeRTOS / NimBLE");
    Serial.println("  Manette BLE pilotée par la tête (MPU9250)");
    Serial.println("==============================================\n");

    /* 1. LittleFS (un échec de montage est loggé, non fatal — le catchall
          du serveur web affichera la procédure `pio run -t uploadfs`) */
    if (!LittleFS.begin(true)) {
        Serial.println("[MAIN] ERREUR : échec de montage LittleFS !");
    } else {
        Serial.println("[MAIN] LittleFS monté (" +
                       String(LittleFS.usedBytes() / 1024) + " Ko utilisés)");
    }

    /* 2. Module métier : réglages NVS + MPU9250 + calibrage gyroscopique
          (l'appareil doit rester immobile ~2 s pendant cette phase) */
    g_headTracker.begin();

    /* 3. Serveur web : AP + STA + portail captif + routes + tâche FreeRTOS.
          Initialisé AVANT NimBLE : le Wi-Fi réserve ses buffers en premier. */
    g_webServer.begin();

    /* 4. Manette BLE HID : services HID + advertising « BOB BL AR » */
    g_bleGamepad.begin();

    /* 4bis. Tare par retour de force : l'ordre de rumble de l'hôte (HID
              Output Report) recentre le point 0 — canal natif BLE, sans
              Wi-Fi (cf. docs/TARE_FORCE_FEEDBACK.md). */
    g_bleGamepad.setRumbleHandler(_onRumbleOrder);

    /* 5. Tâche temps réel 100 Hz : lecture IMU → fusion Mahony → mapping
          → jerk → rapport BLE HID (priorité 4, au-dessus du serveur web) */
    g_headTracker.startTask();

    Serial.println("[MAIN] Démarrage terminé.");
    Serial.println("[MAIN] Interface web : Wi-Fi « " + String(AP_SSID) +
                   " » → http://" + String(AP_IP) + "/");
    Serial.println("[MAIN] Appairage manette : « " + String(BLE_DEVICE_NAME) +
                   " » dans les réglages Bluetooth de macOS / Windows.");
}

void loop() {
    /* Vide : toute la logique vit dans les tâches FreeRTOS */
    vTaskDelay(pdMS_TO_TICKS(1000));
}
