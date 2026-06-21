#include "wled.h"
#include <HTTPClient.h>

// PixC API host the device calls to learn its MQTT broker (see provision()).
// In prod this is the fixed API domain; in dev it's the Mac LAN IP, pushed by
// the app during pairing via usermod config (um.PixcConnect.apiHost). The
// broker location itself is NOT hardcoded — it is fetched from the API.
#ifndef PIXC_API_HOST
  #define PIXC_API_HOST "api.pixc.app"
#endif
#ifndef PIXC_API_PORT
  #define PIXC_API_PORT 8080
#endif
// Last-resort broker fallback ONLY if the API can't be reached at all.
#ifndef PIXC_MQTT_HOST
  #define PIXC_MQTT_HOST ""
#endif
#ifndef PIXC_MQTT_PORT
  #define PIXC_MQTT_PORT 1883
#endif

// PixC device-edge usermod. Two jobs:
//
//  1. MQTT bridge to the PixC cloud (pixc-mqtt broker). WLED's native MQTT
//     topics/payloads don't match the PixC contract, so this usermod:
//       - forces the device topic to `pixc/d/{mac}` (full MAC, lowercase) so
//         WLED subscribes `pixc/d/{mac}/api` for commands;
//       - publishes `announce` (once on connect), `state` and `health` to
//         `pixc/d/{mac}/{kind}` in the shapes pixc-mqtt expects.
//
//  2. On the first successful MQTT (cloud) connect after Wi-Fi is up — i.e.
//     right after initial setup / pairing — flash the strip solid green for
//     3 seconds, then restore the previous color / brightness / effect.
//
// Requires MQTT compiled in (do NOT set WLED_DISABLE_MQTT).
class PixcConnectBlink : public Usermod {
  private:
    bool _blinkDone = false;   // green blink only on the first connect
    bool _flashing = false;
    bool _announced = false;
    unsigned long _start = 0;
    unsigned long _lastState = 0;
    unsigned long _lastHealth = 0;
    byte _savedCol[4] = {0, 0, 0, 0};
    byte _savedBri = 0;
    uint8_t _savedFx = 0;

    // PixC API host (provisioned by the app, persisted in usermod config). The
    // device calls GET {apiHost}:{apiPort}/api/v1/provision?mac=... to learn the
    // MQTT broker, then hands off to MQTT.
    String _apiHost = PIXC_API_HOST;
    uint16_t _apiPort = PIXC_API_PORT;
    bool _provisioned = false;          // got the broker from the API yet?
    unsigned long _lastProvisionTry = 0;

    static constexpr unsigned long kStateIntervalMs  = 5000;
    static constexpr unsigned long kHealthIntervalMs = 30000;
    static constexpr unsigned long kProvisionRetryMs = 10000;

    // Ask the PixC API which broker to use, then point WLED's MQTT at it. The
    // broker is dynamic (dev LAN IP rotates), so it is never hardcoded — fetched.
    void fetchProvisionConfig() {
      if (WiFi.status() != WL_CONNECTED || _apiHost.length() == 0) return;
      char url[200];
      snprintf(url, sizeof(url), "http://%s:%u/api/v1/provision?mac=%s",
               _apiHost.c_str(), _apiPort, escapedMac.c_str());
      WiFiClient client;
      HTTPClient http;
      http.setConnectTimeout(4000);
      if (!http.begin(client, url)) return;
      int code = http.GET();
      if (code == 200) {
        DynamicJsonDocument doc(640);
        if (!deserializeJson(doc, http.getString())) {
          JsonObject d = doc["data"].isNull() ? doc.as<JsonObject>()
                                              : doc["data"].as<JsonObject>();
          // API serializes snake_case (mqtt_host/mqtt_port); accept camelCase too.
          const char* host = d["mqtt_host"] | (d["mqttHost"] | "");
          int port = d["mqtt_port"] | (d["mqttPort"] | PIXC_MQTT_PORT);
          if (host && strlen(host) > 0) {
            strlcpy(mqttServer, host, MQTT_MAX_SERVER_LEN + 1);
            mqttPort = port;
            mqttEnabled = true;
            _provisioned = true;
            DEBUG_PRINTF("[PixC] broker from API: %s:%d\n", mqttServer, mqttPort);
            // Force WLED to (re)connect MQTT to the new broker.
            if (mqtt != nullptr && mqtt->connected()) mqtt->disconnect();
            initMqtt();
          }
        }
      }
      http.end();
    }

    void publishKind(const char* kind, const char* payload) {
      if (!WLED_MQTT_CONNECTED) return;
      char topic[MQTT_MAX_TOPIC_LEN + 16];
      snprintf(topic, sizeof(topic), "%s/%s", mqttDeviceTopic, kind);
      mqtt->publish(topic, 0, false, payload);
    }

    void publishAnnounce() {
      char buf[160];
      const char* ledType = strip.hasWhiteChannel() ? "SK6812" : "WS2812B";
      snprintf(buf, sizeof(buf),
        "{\"fw_version\":\"%s\",\"led_type\":\"%s\",\"led_count\":%u}",
        versionString, ledType, strip.getLengthTotal());
      publishKind("announce", buf);
    }

    void publishState() {
      char buf[224];
      Segment& seg = strip.getSegment(strip.getMainSegmentId());
      // WLED-native shape (pixc-mqtt prefers on/bri/seg[].col/fx).
      snprintf(buf, sizeof(buf),
        "{\"on\":%s,\"bri\":%u,\"seg\":[{\"col\":[[%u,%u,%u,%u]],\"fx\":%u,\"pal\":%u,\"sx\":%u,\"ix\":%u}]}",
        (bri > 0 ? "true" : "false"), bri,
        colPri[0], colPri[1], colPri[2], colPri[3], effectCurrent,
        seg.palette, seg.speed, seg.intensity);
      publishKind("state", buf);
    }

    void publishHealth() {
      char buf[256];
      String ssid = WiFi.SSID();
      String ip = WiFi.localIP().toString();
      int signal = constrain(2 * (WiFi.RSSI() + 100), 0, 100);
      snprintf(buf, sizeof(buf),
        "{\"rssi\":%d,\"signal\":%d,\"ssid\":\"%s\",\"ip\":\"%s\",\"free_heap\":%u,\"uptime\":%lu,\"fw_version\":\"%s\"}",
        (int)WiFi.RSSI(), signal, ssid.c_str(), ip.c_str(),
        (unsigned)ESP.getFreeHeap(),
        (unsigned long)(millis() / 1000), versionString);
      publishKind("health", buf);
    }

    // Apply a WLED config fragment pushed by the cloud over `pixc/d/{mac}/cfg`
    // (power plan -> def.bri, restore-on-power -> def.on, slow-fade -> light.tr.dur).
    // Only native WLED cfg keys take effect; unknown keys are ignored safely.
    void applyCfg(const char* payload) {
      // This WLED fork uses ArduinoJson v6 — JsonDocument is abstract; use a
      // sized DynamicJsonDocument. The cfg fragment (def/light/pixc) is small.
      DynamicJsonDocument doc(2048);
      if (deserializeJson(doc, payload)) return;
      JsonObject root = doc.as<JsonObject>();
      deserializeConfig(root, false);
      serializeConfigToFS();
    }

    // Factory reset: wipe config + Wi-Fi credentials and reboot back into PixC-AP.
    void factoryReset() {
      WLED_FS.remove("/cfg.json");
      WLED_FS.remove("/wsec.json");
      doReboot = true;
    }

  public:
    void setup() override {
      // PixC-AP is a recovery hotspot, not an always-on beacon: once the device
      // joins the home Wi-Fi the AP is torn down (WLED shuts it on connect for
      // any non-ALWAYS behaviour). It only (re)opens when the station can't
      // connect — after the ~30s grace below — so a mispaired/dropped device is
      // still reachable for re-provisioning.
      apBehavior = AP_BEHAVIOR_NO_CONN;
      mqttEnabled = true;
      // Broker is NOT hardcoded — it is fetched from the PixC API (see
      // fetchProvisionConfig) on every Wi-Fi connect. Only seed the last-resort
      // fallback if a build-time broker was explicitly provided.
      if (strlen(PIXC_MQTT_HOST) > 0 && strlen(mqttServer) == 0) {
        strlcpy(mqttServer, PIXC_MQTT_HOST, MQTT_MAX_SERVER_LEN + 1);
        mqttPort = PIXC_MQTT_PORT;
      }
      // Force the PixC topic scheme before MQTT connects so WLED subscribes
      // pixc/d/{mac}/api and publishes its LWT under the same prefix.
      // "pixc/d/" (7) + 12-char MAC = 19 chars, within MQTT_MAX_TOPIC_LEN (32).
      snprintf(mqttDeviceTopic, MQTT_MAX_TOPIC_LEN + 1, "pixc/d/%s",
               escapedMac.c_str());
    }

    // Called by WLED when the station connects to Wi-Fi — fetch the broker.
    void connected() override {
      _provisioned = false;
      _lastProvisionTry = 0;   // provision asap in loop()
    }

    // Persist the PixC API host/port so the app provisions it once and it
    // survives reboots (cfg.json -> um.PixcConnect.{apiHost,apiPort}).
    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject("PixcConnect");
      top["apiHost"] = _apiHost;
      top["apiPort"] = _apiPort;
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root["PixcConnect"];
      if (top.isNull()) return false;
      _apiHost = top["apiHost"] | _apiHost;
      _apiPort = top["apiPort"] | _apiPort;
      return true;
    }

    void onMqttConnect(bool /*sessionPresent*/) override {
      _announced = false; // re-announce on every (re)connect

      // Subscribe the PixC control subtopics the core doesn't handle. `/api`
      // (state commands) is subscribed by WLED core; `/cfg` carries config
      // fragments (power plan, restore-on-power, default brightness, slow-fade)
      // and `/reset` triggers a factory wipe.
      if (mqtt != nullptr) {
        String base = mqttDeviceTopic;
        mqtt->subscribe((base + "/cfg").c_str(), 0);
        mqtt->subscribe((base + "/reset").c_str(), 0);
      }

      if (!_blinkDone) {
        _blinkDone = true;
        memcpy(_savedCol, colPri, 4);
        _savedBri = bri;
        _savedFx = effectCurrent;
        colPri[0] = 0; colPri[1] = 255; colPri[2] = 0; colPri[3] = 0;
        effectCurrent = FX_MODE_STATIC;
        bri = 255;
        colorUpdated(CALL_MODE_DIRECT_CHANGE);
        _start = millis();
        _flashing = true;
      }
    }

    bool onMqttMessage(char* topic, char* payload) override {
      String base = mqttDeviceTopic;
      if (strstr(topic, (base + "/cfg").c_str()) != nullptr) {
        applyCfg(payload);
        return true;
      }
      if (strstr(topic, (base + "/reset").c_str()) != nullptr) {
        factoryReset();
        return true;
      }
      return false;
    }

    void loop() override {
      const unsigned long now = millis();

      if (_flashing && (now - _start > 3000)) {
        memcpy(colPri, _savedCol, 4);
        bri = _savedBri;
        effectCurrent = _savedFx;
        colorUpdated(CALL_MODE_DIRECT_CHANGE);
        _flashing = false;
      }

      // Once on Wi-Fi, fetch the broker from the PixC API (retry until it
      // sticks). Broker is never hardcoded — this is the device→Java handoff.
      if (!_provisioned && WiFi.status() == WL_CONNECTED &&
          (_lastProvisionTry == 0 || now - _lastProvisionTry >= kProvisionRetryMs)) {
        _lastProvisionTry = now;
        fetchProvisionConfig();
      }

      if (!WLED_MQTT_CONNECTED) return;

      if (!_announced) {
        _announced = true;
        publishAnnounce();
        publishState();
        _lastState = now;
        _lastHealth = now;
      }
      if (now - _lastState >= kStateIntervalMs) {
        _lastState = now;
        publishState();
      }
      if (now - _lastHealth >= kHealthIntervalMs) {
        _lastHealth = now;
        publishHealth();
      }
    }

    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};

static PixcConnectBlink pixc_connect_blink;
REGISTER_USERMOD(pixc_connect_blink);
