#include "web_server.h"
#include "git_version.h"
#include "head_tracker.h"
#include "ble_gamepad.h"
#include "imu_mpu9250.h"
#include <esp_system.h>
#include <esp_wifi.h>   /* esp_wifi_set_country_code */

AppWebServer g_webServer;

volatile bool AppWebServer::_staGotIp = false;
volatile bool AppWebServer::_staFailed = false;

/* ------------------------------------------------------------------ */
/* Démarrage AP + reconnexion STA                                      */
/* ------------------------------------------------------------------ */

void AppWebServer::begin() {
    /* 0. Pré-configuration radio. Le code pays « FR » débloque les canaux
          1-13 (le défaut « monde » du pilote ignore 12/13, utilisés par les
          box françaises → réseaux invisibles au scan). L'économie d'énergie
          modem est coupée pour la fiabilité de la coexistence AP+STA. */
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    esp_wifi_set_country_code(WIFI_COUNTRY_CODE, true);

    /* 0bis. Scan de démarrage EN MODE STA SEUL : c'est un scan « premier
          plan », le plus fiable (avec SoftAP actif, le scan passe en
          « arrière-plan », contraint par les retours au canal de l'AP).
          Le résultat est servi instantanément au premier clic « Scanner »
          de l'UI ; il sert aussi de diagnostic : si ce scan voit des
          réseaux mais pas les scans à chaud, le problème est côté activité
          web ; s'il voit 0 réseau, c'est l'environnement radio/pilote. */
    Serial.println("[WEB] Scan de démarrage…");
    WiFi.setScanTimeout(10000);            /* garde-fou : 10 s max */
    const int16_t bootScan = WiFi.scanNetworks();   /* bloquant ~2 s */
    _bootScanValid = (bootScan > 0);
    Serial.println("[WEB] Scan de démarrage : " + String(bootScan) + " réseau(x)");
    for (int i = 0; i < bootScan && i < 6; i++) {
        Serial.println("      · " + WiFi.SSID(i) + "  (" + String(WiFi.RSSI(i)) + " dBm)");
    }

    /* 1. Point d'accès avec IP statique (interface toujours à 192.168.4.1) */
    WiFi.mode(WIFI_AP_STA);
    IPAddress apIp, apNetmask;
    apIp.fromString(AP_IP);
    apNetmask.fromString("255.255.255.0");
    WiFi.softAPConfig(apIp, apIp, apNetmask);
    WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CLIENTS);
    WiFi.setAutoReconnect(true);

    /* 2. Événements Wi-Fi asynchrones → flags volatils (statut STA) */
    WiFi.onEvent(_onWiFiEvent);

    Serial.println("[WEB] Point d'accès démarré : " + String(AP_SSID));
    Serial.println("[WEB] IP : " + WiFi.softAPIP().toString());

    /* 3. Reconnexion au dernier réseau mémorisé (NVS, non bloquante) */
    Preferences prefs;
    prefs.begin("wifi", true);
    String savedSSID = prefs.getString("ssid", "");
    String savedPass = prefs.getString("pass", "");
    prefs.end();
    if (savedSSID.length() > 0) {
        Serial.println("[WEB] Tentative de connexion à : " + savedSSID);
        WiFi.begin(savedSSID.c_str(), savedPass.c_str());
    }

    /* 4. Routes, WebSocket, DNS captif, démarrage serveur */
    _setupRoutes();
    _setupWebSocket();
    _setupCaptiveDNS();
    _server.begin();
    Serial.println("[WEB] Serveur HTTP démarré sur le port 80");

    /* 5. Tâche FreeRTOS (DNS + nettoyage WS + télémétrie périodique) */
#if CONFIG_FREERTOS_UNICORE
    xTaskCreate(_taskEntry, "WebServer", STACK_SIZE_WEB, this, PRIORITY_WEB, nullptr);
#else
    xTaskCreatePinnedToCore(_taskEntry, "WebServer", STACK_SIZE_WEB, this,
                            PRIORITY_WEB, nullptr, 0);
#endif
}

void AppWebServer::_onWiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            _staGotIp = true;
            _staFailed = false;
            Serial.println("[WEB] STA connecté — IP : " + WiFi.localIP().toString());
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            _staGotIp = false;
            _staFailed = true;
            Serial.println("[WEB] STA déconnecté");
            break;
        default:
            break;
    }
}

void AppWebServer::_setupCaptiveDNS() {
    IPAddress apIp;
    apIp.fromString(AP_IP);
    _dns.start(53, "*", apIp);
    Serial.println("[WEB] DNS captif démarré (port 53, redirection → " + String(AP_IP) + ")");
}

/* ------------------------------------------------------------------ */
/* Routes HTTP                                                         */
/* ------------------------------------------------------------------ */

void AppWebServer::_setupRoutes() {
    const String root = "http://" + String(AP_IP) + "/";

    /* ============== PORTAIL CAPTIF (détection OS — AVANT serveStatic) ==============
     * L'OS du client interroge ces URLs pour tester l'accès Internet ;
     * un 302 vers notre IP déclenche l'ouverture automatique du portail. */
    /* NOTE : capture PAR VALEUR obligatoire — le handler est appelé bien après
     * le retour de _setupRoutes() (tâche async_tcp) ; une capture par référence
     * pendrait sur un String détruit → redirections aléatoires / crash. */
    auto captiveRedirect = [root](AsyncWebServerRequest* request) {
        request->redirect(root);
    };
    _server.on("/generate_204", HTTP_GET, captiveRedirect);              /* Android */
    _server.on("/gen_204", HTTP_GET, captiveRedirect);
    _server.on("/hotspot-detect.html", HTTP_GET, captiveRedirect);
    _server.on("/library/test/success.html", HTTP_GET, captiveRedirect); /* Apple */
    _server.on("/connecttest.txt", HTTP_GET, captiveRedirect);           /* Windows NCSI */
    _server.on("/ncsi.txt", HTTP_GET, captiveRedirect);                  /* Windows NCSI (legacy) */
    _server.on("/redirect", HTTP_GET, captiveRedirect);
    _server.on("/canonical.html", HTTP_GET, captiveRedirect);            /* Firefox */
    _server.on("/success.txt", HTTP_GET, captiveRedirect);               /* Firefox detectportal */
    _server.on("/kindle-wifi/wifistub.html", HTTP_GET, captiveRedirect); /* Kindle */

    /* ===================== API SYSTÈME ===================== */
    _server.on("/api/system/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleSystemStatus(request);
    });
    _server.on("/api/system/restart", HTTP_POST, [this](AsyncWebServerRequest* request) {
        _handleSystemRestart(request);
    });

    /* ===================== API WI-FI ===================== */
    _server.on("/api/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleWifiScan(request);
    });
    _server.on("/api/wifi/connect", HTTP_POST, [this](AsyncWebServerRequest* request) {
        _handleWifiConnect(request);
    });

    /* ===================== API MANETTE ===================== */
    _server.on("/api/gamepad/config", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleGamepadConfigGet(request);
    });
    _server.on("/api/gamepad/config", HTTP_POST, [this](AsyncWebServerRequest* request) {
        _handleGamepadConfigPost(request);
    });
    _server.on("/api/gamepad/tare", HTTP_POST, [this](AsyncWebServerRequest* request) {
        _handleGamepadTare(request);
    });

    /* ===================== API BLUETOOTH ===================== */
    _server.on("/api/ble/name", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleBleNameGet(request);
    });
    _server.on("/api/ble/name", HTTP_POST, [this](AsyncWebServerRequest* request) {
        _handleBleNamePost(request);
    });

    /* Fichiers statiques : la racine sert index.html depuis LittleFS */
    _server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    /* ========== CATCHALL — redirection des requêtes « captivées » ==========
     * Toute requête dont le Host n'est pas l'IP de l'AP a été détournée par
     * le DNS captif → redirect vers l'interface. Host correct + LittleFS
     * vide → page de secours (instructions d'upload). */
    const String expectedHost = String(AP_IP);
    const String expectedHostPort = String(AP_IP) + ":80";
    _server.onNotFound([this, expectedHost, expectedHostPort, root](AsyncWebServerRequest* request) {
        String host = request->getHeader("Host") ? request->getHeader("Host")->value() : "";
        if (host != expectedHost && host != expectedHostPort) {
            request->redirect(root);
            return;
        }
        String path = request->url();
        if (path == "/" || path == "/index.html" || path == "/index.htm") {
            request->send(200, "text/html",
                "<!DOCTYPE html><html lang='fr'><head><meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>BOB BL AR</title><style>"
                "body{background:#131417;color:#e0e0e0;font-family:monospace;"
                "display:flex;align-items:center;justify-content:center;height:100vh;margin:0}"
                "</style></head><body><div style='text-align:center'>"
                "<h1>LittleFS vide</h1>"
                "<p>Ex&eacute;cutez&nbsp;: pio run -t uploadfs</p>"
                "</div></body></html>");
            return;
        }
        request->send(404, "text/plain", "Not Found");
    });
}

/* ------------------------------------------------------------------ */
/* Handlers REST — système                                             */
/* ------------------------------------------------------------------ */

void AppWebServer::_handleSystemStatus(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["version"] = GIT_VERSION;
    doc["uptimeMs"] = (uint32_t)millis();
    doc["heapFree"] = ESP.getFreeHeap();
    doc["heapMin"] = ESP.getMinFreeHeap();

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["apSsid"] = AP_SSID;
    wifi["apIp"] = WiFi.softAPIP().toString();
    wifi["apClients"] = WiFi.softAPgetStationNum();

    Preferences prefs;
    prefs.begin("wifi", true);
    const String staSsid = prefs.getString("ssid", "");
    prefs.end();
    wifi["staSsid"] = staSsid;
    if (_staGotIp && WiFi.status() == WL_CONNECTED) {
        wifi["staState"] = "connected";
        wifi["staIp"] = WiFi.localIP().toString();
        wifi["staRssi"] = WiFi.RSSI();
    } else if (_staFailed) {
        wifi["staState"] = "failed";
    } else if (staSsid.length() > 0) {
        wifi["staState"] = "connecting";
    } else {
        wifi["staState"] = "idle";
    }

    JsonObject ble = doc["ble"].to<JsonObject>();
    ble["name"] = g_bleGamepad.deviceName();
    ble["connected"] = g_bleGamepad.isConnected();
    ble["clients"] = g_bleGamepad.clientCount();

    JsonObject imu = doc["imu"].to<JsonObject>();
    imu["ok"] = g_headTracker.imuOk();
    imu["whoami"] = String(g_imu.whoAmI(), HEX);
    imu["mag"] = g_imu.magOk();
    imu["tempC"] = (int)(g_imu.lastTempC() * 10) / 10.0;

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void AppWebServer::_handleSystemRestart(AsyncWebServerRequest* request) {
    request->send(200, "application/json",
                  "{\"status\":\"ok\",\"message\":\"Redémarrage dans 3 s\"}");
    /* Convention du socle : action différée en tâche détachée, jamais
       ESP.restart() dans le handler (la réponse doit partir d'abord). */
    xTaskCreate(_restartTaskEntry, "Restart", 2048, nullptr, 1, nullptr);
}

void AppWebServer::_restartTaskEntry(void*) {
    Serial.println("[WEB] Redémarrage différé (3 s)…");
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP.restart();
    vTaskDelete(nullptr);
}

/* ------------------------------------------------------------------ */
/* Handlers REST — Wi-Fi                                               */
/* ------------------------------------------------------------------ */

void AppWebServer::_handleWifiScan(AsyncWebServerRequest* request) {
    /* CORRECTIF CRASH : la version précédente pilotait le scan depuis ce
       handler (tâche async_tcp) → débordement de pile → exception. Désormais
       ce handler ne touche NI au Wi-Fi NI à la NVS : il transmet la demande
       à la tâche web (machine à états _advanceScan) et attend. */

    /* Premier scan après démarrage : résultat du scan de boot, instantané */
    if (_bootScanValid) {
        _bootScanValid = false;
        _scanBuildResults();
        request->send(200, "application/json", _scanJson);
        return;
    }

    if (_scanState == SCAN_REQUESTED || _scanState == SCAN_SCANNING) {
        request->send(503, "application/json",
                      "{\"error\":\"Un scan est déjà en cours\"}");
        return;
    }
    _scanState = SCAN_REQUESTED;

    /* Attente du résultat (max 12 s) — delay() cède le CPU aux autres tâches */
    unsigned long start = millis();
    while ((_scanState == SCAN_REQUESTED || _scanState == SCAN_SCANNING) &&
           (millis() - start) < 12000) {
        delay(10);
    }

    if (_scanState == SCAN_DONE) {
        request->send(200, "application/json", _scanJson);
    } else if (_scanState == SCAN_FAILED) {
        request->send(500, "application/json",
                      "{\"error\":\"Scan impossible — pilote Wi-Fi occupé (voir moniteur série)\"}");
    } else {
        request->send(500, "application/json", "{\"error\":\"Délai de scan dépassé\"}");
    }
    _scanState = SCAN_IDLE;
}

void AppWebServer::_handleWifiConnect(AsyncWebServerRequest* request) {
    if (!request->hasParam("ssid", true)) {
        request->send(400, "application/json", "{\"error\":\"Paramètre 'ssid' manquant\"}");
        return;
    }
    if (_connState == CONN_REQUESTED || _connState == CONN_CONNECTING) {
        request->send(503, "application/json",
                      "{\"error\":\"Une connexion est déjà en cours\"}");
        return;
    }

    /* NVS + WiFi.begin() sont pilotés par la tâche web (_advanceConnect) :
       ce handler async_tcp n'appelle plus l'API Wi-Fi (cf. correctif scan). */
    _connSsid = request->getParam("ssid", true)->value();
    _connPass = request->hasParam("pass", true)
                ? request->getParam("pass", true)->value() : "";
    _connState = CONN_REQUESTED;

    /* Attente du résultat (max 16 s) — delay() cède le CPU au réseau */
    unsigned long start = millis();
    while ((_connState == CONN_REQUESTED || _connState == CONN_CONNECTING) &&
           (millis() - start) < 16000) {
        delay(50);
    }

    if (_connState == CONN_SUCCESS && WiFi.status() == WL_CONNECTED) {
        const String ip = WiFi.localIP().toString();
        request->send(200, "application/json",
                      "{\"status\":\"connected\",\"ip\":\"" + ip + "\"}");
    } else if (_connState == CONN_FAILED) {
        request->send(200, "application/json",
                      "{\"status\":\"failed\",\"message\":\"Vérifiez le mot de passe\"}");
    } else {
        request->send(200, "application/json",
                      "{\"status\":\"failed\",\"message\":\"Délai de connexion dépassé\"}");
    }
    _connState = CONN_IDLE;
}

/* ------------------------------------------------------------------ */
/* Machines à états Wi-Fi — exécutées par la tâche web                 */
/* ------------------------------------------------------------------ */

void AppWebServer::_advanceScan() {
    if (_scanState == SCAN_REQUESTED) {
        /* Purge d'un éventuel résultat périmé (ex. scan de boot jamais servi) */
        WiFi.scanDelete();

        /* Interrompre une reconnexion STA en boucle : le pilote refuse un scan
           pendant une connexion (ESP_ERR_WIFI_STATE). L'ordre importe :
           suspendre l'auto-reconnexion AVANT disconnect(), sinon l'événement
           DISCONNECTED relancerait WiFi.begin() dans l'instant. */
        if (WiFi.status() == WL_CONNECTED) {
            _scanNeedsReconnect = false;
        } else {
            Preferences prefs;
            prefs.begin("wifi", true);
            _scanReconnectSsid = prefs.getString("ssid", "");
            _scanReconnectPass = prefs.getString("pass", "");
            prefs.end();
            _scanNeedsReconnect = (_scanReconnectSsid.length() > 0);
            if (_scanNeedsReconnect) {
                WiFi.setAutoReconnect(false);
                WiFi.disconnect(false);   /* STA seul — le SoftAP reste actif */
            }
        }
        _scanStartMs = millis();
        _scanLastTryMs = 0;               /* première tentative immédiate */
        _scanAttempts = 0;
        _scanState = SCAN_SCANNING;
        /* Pas de return : tenter le démarrage dès ce passage */
    } else if (_scanState != SCAN_SCANNING) {
        return;
    }

    const int16_t r = WiFi.scanComplete();
    if (r >= 0) {                        /* terminé : JSON + restauration  */
        _scanBuildResults();
        _scanFinish();
        _scanState = SCAN_DONE;
        return;
    }
    if (r == WIFI_SCAN_RUNNING) return;  /* scan en vol : rien à faire     */

    /* r == WIFI_SCAN_FAILED : le pilote refuse de démarrer un scan tant que
       le STA n'a pas fini de se déconnecter (doc IDF, « Scan When Wi-Fi Is
       Connecting ») → réessayer toutes les 150 ms dans la limite du budget. */
    if (millis() - _scanStartMs > WIFI_SCAN_BUDGET_MS) {
        Serial.println("[WEB] Scan abandonné : démarrage refusé " +
                       String(_scanAttempts) + " fois (pilote Wi-Fi occupé)");
        _scanFinish();
        _scanState = SCAN_FAILED;
        return;
    }
    if (millis() - _scanLastTryMs >= 150) {
        _scanLastTryMs = millis();
        _scanAttempts++;
        const int16_t rc = WiFi.scanNetworks(true, false, false, WIFI_SCAN_DWELL_MS);
        if (rc == WIFI_SCAN_RUNNING) {
            Serial.println("[WEB] Scan démarré (essai " + String(_scanAttempts) + ")");
        } else if (_scanAttempts == 1 || _scanAttempts % 10 == 0) {
            Serial.println("[WEB] Démarrage refusé par le pilote (essai " +
                           String(_scanAttempts) + ") — nouvelle tentative…");
        }
    }
}

void AppWebServer::_scanBuildResults() {
    int n = WiFi.scanComplete();
    if (n < 0) n = 0;
    Serial.println("[WEB] Scan Wi-Fi : " + String(n) + " réseau(x)");

    JsonDocument doc;
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
        JsonObject net = arr.add<JsonObject>();
        net["ssid"] = WiFi.SSID(i);
        net["rssi"] = WiFi.RSSI(i);
        net["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }

    _scanJson = String();
    serializeJson(doc, _scanJson);
    WiFi.scanDelete();   /* libère la mémoire du scan */
}

void AppWebServer::_scanFinish() {
    if (_scanNeedsReconnect) {
        _scanNeedsReconnect = false;
        WiFi.setAutoReconnect(true);
        WiFi.begin(_scanReconnectSsid.c_str(), _scanReconnectPass.c_str());
        _scanReconnectSsid = String();
        _scanReconnectPass = String();
    }
}

void AppWebServer::_advanceConnect() {
    switch (_connState) {
        case CONN_REQUESTED: {
            /* Sauvegarde NVS : identifiants mémorisés pour le prochain démarrage */
            Preferences prefs;
            prefs.begin("wifi", false);
            prefs.putString("ssid", _connSsid);
            prefs.putString("pass", _connPass);
            prefs.end();

            /* RÈGLE D'OR : jamais WiFi.disconnect() en mode AP_STA — cela
               tuerait le SoftAP. WiFi.begin() remplace proprement le STA. */
            _staGotIp = false;
            _staFailed = false;
            WiFi.begin(_connSsid.c_str(), _connPass.c_str());
            _connStartMs = millis();
            _connState = CONN_CONNECTING;
            Serial.println("[WEB] Connexion STA demandée : " + _connSsid);
            break;
        }
        case CONN_CONNECTING:
            if (_staGotIp && WiFi.status() == WL_CONNECTED) {
                _connState = CONN_SUCCESS;
            } else if (_staFailed || (millis() - _connStartMs) > 15000) {
                _connState = CONN_FAILED;
            }
            break;
        default:
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Handlers REST — manette                                             */
/* ------------------------------------------------------------------ */

void AppWebServer::_handleGamepadConfigGet(AsyncWebServerRequest* request) {
    const GamepadSettings& s = g_headTracker.settings();

    JsonDocument doc;
    doc["dzPitch"] = s.deadzonePitchDeg;
    doc["dzRoll"] = s.deadzoneRollDeg;
    doc["dzYaw"] = s.deadzoneYawDeg;
    doc["fullDeflectPitch"] = s.fullDeflectPitchDeg;
    doc["fullDeflectRoll"] = s.fullDeflectRollDeg;
    doc["fullDeflectYaw"] = s.fullDeflectYawDeg;
    /* Alias legacy : un navigateur gardant l'ancien app.js en cache
       requête encore « fullDeflect » — il reçoit la valeur du tangage. */
    doc["fullDeflect"] = s.fullDeflectPitchDeg;
    doc["jerkThresh"] = s.jerkThresholdMs2;
    doc["jerkCooldown"] = s.jerkCooldownMs;
    doc["invX"] = s.invertX;
    doc["invY"] = s.invertY;
    doc["invZ"] = s.invertZ;

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void AppWebServer::_handleGamepadConfigPost(AsyncWebServerRequest* request) {
    /* Convention : champ absent = valeur courante conservée */
    GamepadSettings s = g_headTracker.settings();

    if (request->hasParam("dzPitch", true))
        s.deadzonePitchDeg = request->getParam("dzPitch", true)->value().toFloat();
    if (request->hasParam("dzRoll", true))
        s.deadzoneRollDeg = request->getParam("dzRoll", true)->value().toFloat();
    if (request->hasParam("dzYaw", true))
        s.deadzoneYawDeg = request->getParam("dzYaw", true)->value().toFloat();
    /* Sensibilité par axe ; à défaut, l'ancien paramètre unique « fullDeflect »
       (app.js en cache) règle les trois axes d'un coup. */
    if (request->hasParam("fullDeflectPitch", true))
        s.fullDeflectPitchDeg = request->getParam("fullDeflectPitch", true)->value().toFloat();
    else if (request->hasParam("fullDeflect", true))
        s.fullDeflectPitchDeg = request->getParam("fullDeflect", true)->value().toFloat();
    if (request->hasParam("fullDeflectRoll", true))
        s.fullDeflectRollDeg = request->getParam("fullDeflectRoll", true)->value().toFloat();
    else if (request->hasParam("fullDeflect", true))
        s.fullDeflectRollDeg = request->getParam("fullDeflect", true)->value().toFloat();
    if (request->hasParam("fullDeflectYaw", true))
        s.fullDeflectYawDeg = request->getParam("fullDeflectYaw", true)->value().toFloat();
    else if (request->hasParam("fullDeflect", true))
        s.fullDeflectYawDeg = request->getParam("fullDeflect", true)->value().toFloat();
    if (request->hasParam("jerkThresh", true))
        s.jerkThresholdMs2 = request->getParam("jerkThresh", true)->value().toFloat();
    if (request->hasParam("jerkCooldown", true))
        s.jerkCooldownMs = (uint16_t)request->getParam("jerkCooldown", true)->value().toInt();

    auto parseBool = [request](const char* name, bool current) -> bool {
        if (!request->hasParam(name, true)) return current;
        const String v = request->getParam(name, true)->value();
        return (v == "1" || v == "true" || v == "on");
    };
    s.invertX = parseBool("invX", s.invertX);
    s.invertY = parseBool("invY", s.invertY);
    s.invertZ = parseBool("invZ", s.invertZ);

    /* 1. Validation → 400 immédiat si hors bornes */
    auto bad = [](float v, float lo, float hi) { return (v < lo || v > hi); };
    if (bad(s.deadzonePitchDeg, GP_DEADZONE_MIN_DEG, GP_DEADZONE_MAX_DEG) ||
        bad(s.deadzoneRollDeg,  GP_DEADZONE_MIN_DEG, GP_DEADZONE_MAX_DEG) ||
        bad(s.deadzoneYawDeg,   GP_DEADZONE_MIN_DEG, GP_DEADZONE_MAX_DEG)) {
        request->send(400, "application/json",
                      "{\"error\":\"Zone morte invalide (" + String((int)GP_DEADZONE_MIN_DEG) +
                      "-" + String((int)GP_DEADZONE_MAX_DEG) + "°)\"}");
        return;
    }
    if (bad(s.fullDeflectPitchDeg, GP_FULL_DEFLECT_MIN_DEG, GP_FULL_DEFLECT_MAX_DEG) ||
        bad(s.fullDeflectRollDeg,  GP_FULL_DEFLECT_MIN_DEG, GP_FULL_DEFLECT_MAX_DEG) ||
        bad(s.fullDeflectYawDeg,   GP_FULL_DEFLECT_MIN_DEG, GP_FULL_DEFLECT_MAX_DEG)) {
        request->send(400, "application/json",
                      "{\"error\":\"Sensibilité invalide (" + String((int)GP_FULL_DEFLECT_MIN_DEG) +
                      "-" + String((int)GP_FULL_DEFLECT_MAX_DEG) + "°)\"}");
        return;
    }
    if (bad(s.jerkThresholdMs2, GP_JERK_TH_MIN_MS2, GP_JERK_TH_MAX_MS2)) {
        request->send(400, "application/json",
                      "{\"error\":\"Seuil jerk invalide (" + String((int)GP_JERK_TH_MIN_MS2) +
                      "-" + String((int)GP_JERK_TH_MAX_MS2) + " m/s²)\"}");
        return;
    }
    if (s.jerkCooldownMs < GP_COOLDOWN_MIN_MS || s.jerkCooldownMs > GP_COOLDOWN_MAX_MS) {
        request->send(400, "application/json",
                      "{\"error\":\"Recharge jerk invalide (" + String(GP_COOLDOWN_MIN_MS) +
                      "-" + String(GP_COOLDOWN_MAX_MS) + " ms)\"}");
        return;
    }

    /* 2. Application à chaud + NVS vérifiée par relecture */
    if (!g_headTracker.applySettings(s)) {
        request->send(500, "application/json", "{\"error\":\"Ecriture NVS non confirmée\"}");
        return;
    }

    /* 3. Réponse + log */
    JsonDocument doc;
    doc["status"] = "ok";
    doc["dzPitch"] = s.deadzonePitchDeg;
    doc["dzRoll"] = s.deadzoneRollDeg;
    doc["dzYaw"] = s.deadzoneYawDeg;
    doc["fullDeflectPitch"] = s.fullDeflectPitchDeg;
    doc["fullDeflectRoll"] = s.fullDeflectRollDeg;
    doc["fullDeflectYaw"] = s.fullDeflectYawDeg;
    doc["jerkThresh"] = s.jerkThresholdMs2;
    doc["jerkCooldown"] = s.jerkCooldownMs;
    doc["invX"] = s.invertX;
    doc["invY"] = s.invertY;
    doc["invZ"] = s.invertZ;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void AppWebServer::_handleGamepadTare(AsyncWebServerRequest* request) {
    if (!g_headTracker.imuOk()) {
        request->send(500, "application/json",
                      "{\"error\":\"Capteur MPU9250 indisponible\"}");
        return;
    }
    const bool ok = g_headTracker.tare();
    if (ok) {
        request->send(200, "application/json",
                      "{\"status\":\"ok\",\"message\":\"Point 0 recalibré\"}");
    } else {
        request->send(500, "application/json",
                      "{\"error\":\"Ecriture NVS non confirmée\"}");
    }
}

/* ------------------------------------------------------------------ */
/* Nom Bluetooth (interface web)                                        */
/* ------------------------------------------------------------------ */

void AppWebServer::_handleBleNameGet(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["name"] = g_bleGamepad.deviceName();
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void AppWebServer::_handleBleNamePost(AsyncWebServerRequest* request) {
    if (!request->hasParam("name", true)) {
        request->send(400, "application/json",
                      "{\"error\":\"Paramètre 'name' manquant\"}");
        return;
    }
    const String name = request->getParam("name", true)->value();
    if (g_bleGamepad.setDeviceName(name)) {
        JsonDocument doc;
        doc["status"] = "ok";
        doc["name"] = g_bleGamepad.deviceName();
        doc["message"] = "Nom Bluetooth enregistré";
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    } else {
        request->send(400, "application/json",
                      "{\"error\":\"Nom invalide (1 à 28 octets)\"}");
    }
}

/* ------------------------------------------------------------------ */
/* WebSocket                                                           */
/* ------------------------------------------------------------------ */

void AppWebServer::_setupWebSocket() {
    _ws.onEvent([this](AsyncWebSocket* server, AsyncWebSocketClient* client,
                       AwsEventType type, void* arg, uint8_t* data, size_t len) {
        _onWSEvent(server, client, type, arg, data, len);
    });
    _server.addHandler(&_ws);
    Serial.println("[WEB] WebSocket initialisé sur /ws");
}

void AppWebServer::_onWSEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                              AwsEventType type, void* arg, uint8_t* data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            Serial.println("[WS] Client connecté : " + String(client->id()));
            break;
        case WS_EVT_DISCONNECT:
            Serial.println("[WS] Client déconnecté : " + String(client->id()));
            break;
        case WS_EVT_DATA:
            /* Les commandes passent par l'API REST ; payload ignoré (log seul).
               NOTE : ESPAsyncWebServer expose WS_EVT_DATA (et non WS_EVT_MESSAGE). */
            Serial.println("[WS] Message reçu (" + String(len) + " octets)");
            break;
        default:
            break;
    }
}

void AppWebServer::broadcast(const String& json) {
    if (_ws.count() > 0) _ws.textAll(json);
}

/* ------------------------------------------------------------------ */
/* Tâche FreeRTOS : DNS + nettoyage WS + télémétrie 10 Hz              */
/* ------------------------------------------------------------------ */

void AppWebServer::_taskEntry(void* param) {
    static_cast<AppWebServer*>(param)->_taskLoop();
}

void AppWebServer::_taskLoop() {
    TickType_t lastWake = xTaskGetTickCount();
    /* Boucle à 20 ms pour la réactivité du DNS captif ; la télémétrie n'est
       diffusée qu'une fois sur TELEMETRY_PERIOD_MS / 20. */
    const TickType_t period = pdMS_TO_TICKS(20);
    uint32_t tick = 0;
    const uint32_t teleDiv = TELEMETRY_PERIOD_MS / 20;

    char buf[256];

    for (;;) {
        /* 1. DNS captif : répondre aux résolutions des clients AP */
        _dns.processNextRequest();

        /* 2. Nettoyage des clients WebSocket morts */
        _ws.cleanupClients();

        /* 2bis. Machines à états Wi-Fi (scan + connexion STA) — l'API Wi-Fi
           et la NVS ne sont JAMAIS appelées depuis les handlers async_tcp */
        _advanceScan();
        _advanceConnect();

        /* 3. Télémétrie périodique */
        if (++tick >= teleDiv) {
            tick = 0;
            const TrackerTelemetry t = g_headTracker.telemetry();
            snprintf(buf, sizeof(buf),
                     "{\"type\":\"tele\",\"up\":%lu,\"heap\":%u,"
                     "\"yaw\":%.1f,\"pitch\":%.1f,\"roll\":%.1f,"
                     "\"lin\":%.1f,\"j1x\":%.0f,\"j1y\":%.0f,\"j2z\":%.0f,"
                     "\"btn\":%d,\"ble\":%d,\"imu\":%d,\"i2e\":%lu}",
                     (unsigned long)millis(),
                     (unsigned)ESP.getFreeHeap(),
                     t.yawDeg, t.pitchDeg, t.rollDeg,
                     t.linAccMs2, t.j1x, t.j1y, t.j2z,
                     t.btn1 ? 1 : 0, t.bleConnected ? 1 : 0, t.imuOk ? 1 : 0,
                     (unsigned long)t.i2cErrors);
            broadcast(String(buf));
        }

        vTaskDelayUntil(&lastWake, period);   /* période stricte, pas de dérive */
    }
}
