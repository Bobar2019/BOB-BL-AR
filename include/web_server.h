#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "config.h"

/**
 * @brief Serveur web du socle (interface_base.md) adapté à l'ESP32-C6.
 *
 *  - Mode AP+STA simultané, IP statique 192.168.4.1
 *  - Portail captif universel (routes de détection multi-OS + DNS wildcard)
 *  - API REST : /api/system/*, /api/wifi/*, /api/gamepad/*
 *  - WebSocket /ws : télémétrie à 10 Hz (bufferisée côté client + rAF)
 *  - Fichiers statiques servis depuis LittleFS
 *  - Tâche FreeRTOS dédiée (DNS + nettoyage WS + diffusion périodique)
 *
 * NOTE C6 : monocœur — la tâche web tourne en priorité 1, sous la tâche
 * manette (priorité 4), toutes deux sur l'unique cœur.
 */
class AppWebServer {
public:
    AppWebServer() : _server(80), _ws("/ws") {}

    /** @brief AP + STA + routes + DNS + WebSocket + tâche FreeRTOS. */
    void begin();

    /** @brief Traite une requête DNS captive (appelé par la tâche web). */
    void processDNS() { _dns.processNextRequest(); }

    /** @brief Diffuse un message JSON à tous les clients WebSocket. */
    void broadcast(const String& json);

    /** @brief IP du point d'accès (diagnostics). */
    String getAPIP() const { return WiFi.softAPIP().toString(); }

private:
    /* --- Séquence d'initialisation --- */
    void _setupRoutes();
    void _setupCaptiveDNS();
    void _setupWebSocket();

    /* --- Handlers REST --- */
    void _handleSystemStatus(AsyncWebServerRequest* request);
    void _handleSystemRestart(AsyncWebServerRequest* request);
    void _handleWifiScan(AsyncWebServerRequest* request);
    void _handleWifiConnect(AsyncWebServerRequest* request);
    void _handleGamepadConfigGet(AsyncWebServerRequest* request);
    void _handleGamepadConfigPost(AsyncWebServerRequest* request);
    void _handleGamepadTare(AsyncWebServerRequest* request);
    void _handleBleNameGet(AsyncWebServerRequest* request);
    void _handleBleNamePost(AsyncWebServerRequest* request);

    /* --- WebSocket --- */
    void _onWSEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                    AwsEventType type, void* arg, uint8_t* data, size_t len);

    /* --- Tâche FreeRTOS --- */
    static void _taskEntry(void* param);
    void _taskLoop();

    /* --- Redémarrage différé (jamais dans un handler HTTP) --- */
    static void _restartTaskEntry(void* param);

    /* --- Événements Wi-Fi (statut STA pour handlers + télémétrie) --- */
    static void _onWiFiEvent(WiFiEvent_t event);

    /* --- Scan & connexion Wi-Fi : orchestration par la tâche web ----------
       Les handlers async_tcp n'appellent JAMAIS l'API Wi-Fi ni la NVS
       (débordement de pile avéré + appels esp_wifi_* hors de leur contexte
       → exception et redémarrage). Ils demandent, attendent, répondent. */
    enum ScanState : uint8_t {
        SCAN_IDLE,       /* repos                                      */
        SCAN_REQUESTED,  /* demande reçue, non encore prise en charge  */
        SCAN_SCANNING,   /* scan démarré (démarrage retenté au besoin) */
        SCAN_DONE,       /* _scanJson prêt à être servi                */
        SCAN_FAILED      /* scan impossible (interface Wi-Fi occupée…) */
    };
    enum ConnState : uint8_t {
        CONN_IDLE, CONN_REQUESTED, CONN_CONNECTING, CONN_SUCCESS, CONN_FAILED
    };

    void _advanceScan();      /* machine à états scan (appelée par _taskLoop) */
    void _advanceConnect();   /* machine à états connexion STA                */
    void _scanBuildResults(); /* JSON des réseaux détectés → _scanJson        */
    void _scanFinish();       /* restaure la reconnexion STA après le scan    */

    volatile ScanState _scanState = SCAN_IDLE;
    volatile ConnState _connState = CONN_IDLE;
    String   _scanJson;                /* résultat du dernier scan        */
    String   _scanReconnectSsid;       /* identifiants à restaurer        */
    String   _scanReconnectPass;
    bool     _scanNeedsReconnect = false;
    bool     _bootScanValid = false;   /* scan de démarrage pas encore servi */
    uint32_t _scanStartMs = 0;         /* budget total du scan            */
    uint32_t _scanLastTryMs = 0;       /* anti-spam de scanNetworks()     */
    uint8_t  _scanAttempts = 0;        /* tentatives de démarrage du scan */
    String   _connSsid;
    String   _connPass;
    uint32_t _connStartMs = 0;

    AsyncWebServer _server;   /* port 80                */
    AsyncWebSocket _ws;       /* endpoint /ws           */
    DNSServer      _dns;      /* port 53, wildcard      */

    static volatile bool _staGotIp;   /* IP obtenue côté réseau client */
    static volatile bool _staFailed;  /* échec de connexion STA        */
};

/* Instance globale (définie dans web_server.cpp, appelée depuis main.cpp) */
extern AppWebServer g_webServer;

#endif /* WEB_SERVER_H */
