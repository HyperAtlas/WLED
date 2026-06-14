#pragma once

#include "wled.h"
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

/*
 * PixC usermod — bridges WLED to the PixC cloud (pixc-mqtt) over MQTT.
 *
 * Topic contract (device MAC = WLED escapedMac, 12 hex chars):
 *   subscribe  pixc/d/{mac}/cmd      <- {power,brightness(0-100),r,g,b,w,effect}
 *   subscribe  pixc/d/{mac}/ota      <- {job_id,url,sig_url,version,sha256}
 *   publish    pixc/d/{mac}/state    -> {power,brightness(0-100),r,g,b,w,effect,watts}  (every 30s + on change)
 *   publish    pixc/d/{mac}/status   -> "online" (retained) on connect
 *   publish    pixc/d/{mac}/ota/progress -> {job_id,status,percent}
 *
 * Presence: pixc-mqtt refreshes a 65s Redis TTL on each /state message, so simply
 * publishing state every 30s keeps the device "online"; silence => offline.
 *
 * NOTE: register the device in PixC pairing with mac = this device's escapedMac.
 */
class PixCUsermod : public Usermod {
  private:
    static constexpr const char* PIXC_FW_VERSION = "1.0.0";
    String base;                       // pixc/d/{mac}
    String apiBase;                    // PixC API origin, e.g. http://10.0.0.5:8080 (settable in WLED config)
    unsigned long lastPower = 0;
    unsigned long lastHealth = 0;
    static const unsigned long POWER_INTERVAL_MS = 30000;
    static const unsigned long HEALTH_INTERVAL_MS = 60000;
    bool subscribed = false;
    bool provisioned = false;

  public:
    void setup() override {
      base = String("pixc/d/") + escapedMac;
    }

    // Fired when WiFi (re)connects. Device hardcodes only apiBase; it asks the cloud which
    // broker to use, then WLED connects there.
    void connected() override {
      if (!apiBase.isEmpty() && !provisioned) {
        provisionBroker();
      }
    }

    void onMqttConnect(bool sessionPresent) override {
      if (mqtt == nullptr) return;
      mqtt->subscribe((base + "/cmd").c_str(), 0);
      mqtt->subscribe((base + "/ota").c_str(), 0);
      mqtt->publish((base + "/status").c_str(), 0, true, "online");
      subscribed = true;
      publishAnnounce();   // capabilities so the cloud knows what this device is
      publishState();      // initial state
      publishHealth();
      emitEvent("boot", "{}");
    }

    // Publish state immediately on any local change (button, app-direct, automation).
    void onStateChange(uint8_t mode) override {
      if (subscribed && WLED_MQTT_CONNECTED) publishState();
    }

    bool onMqttMessage(char* topic, char* payload) override {
      if (strstr(topic, (base + "/cmd").c_str()) != nullptr) {
        applyCommand(payload);
        return true;
      }
      if (strstr(topic, (base + "/ota").c_str()) != nullptr) {
        handleOta(payload);
        return true;
      }
      return false;
    }

    void loop() override {
      if (!subscribed || !WLED_MQTT_CONNECTED) return;
      unsigned long now = millis();
      if (now - lastPower >= POWER_INTERVAL_MS) { publishPower(); lastPower = now; }
      if (now - lastHealth >= HEALTH_INTERVAL_MS) { publishHealth(); lastHealth = now; }
    }

    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }

  private:
    void applyCommand(const char* payload) {
      JsonDocument in;
      if (deserializeJson(in, payload)) return;

      JsonDocument state;
      state["on"] = in["power"] | true;
      int bri100 = in["brightness"] | 100;
      state["bri"] = (uint8_t) constrain((int) lroundf(bri100 * 2.55f), 0, 255);

      JsonArray seg = state["seg"].to<JsonArray>();
      JsonObject s0 = seg.add<JsonObject>();
      JsonArray col = s0["col"].to<JsonArray>();
      JsonArray c0 = col.add<JsonArray>();
      c0.add((int) (in["r"] | 0));
      c0.add((int) (in["g"] | 0));
      c0.add((int) (in["b"] | 0));
      c0.add((int) (in["w"] | 0));
      s0["fx"] = effectToFx(in["effect"] | "solid");

      JsonObject root = state.as<JsonObject>();
      deserializeState(root, CALL_MODE_DIRECT_CHANGE);
      // deserializeState triggers onStateChange -> publishState() fires automatically (no double publish).
    }

    void publishState() {
      if (mqtt == nullptr || !WLED_MQTT_CONNECTED) return;
      Segment& seg = strip.getMainSegment();
      uint32_t c = seg.colors[0];

      JsonDocument out;
      out["power"] = (bri > 0);
      out["brightness"] = (int) lroundf(bri / 2.55f);
      out["r"] = R(c);
      out["g"] = G(c);
      out["b"] = B(c);
      out["w"] = W(c);
      out["effect"] = fxToEffect(seg.mode);

      String buf;
      serializeJson(out, buf);
      mqtt->publish((base + "/state").c_str(), 0, false, buf.c_str());
    }

    // Capabilities — lets the cloud know what this device is (routed to the devices row).
    void publishAnnounce() {
      if (mqtt == nullptr) return;
      JsonDocument out;
      out["model"] = "wled";
      out["fw_version"] = PIXC_FW_VERSION;
      out["led_type"] = strip.hasWhiteChannel() ? "RGBW" : "RGB";
      out["led_count"] = (int) strip.getLengthTotal();
      out["ip"] = WiFi.localIP().toString();
      String buf; serializeJson(out, buf);
      mqtt->publish((base + "/announce").c_str(), 0, true, buf.c_str());
    }

    // Power telemetry -> TimescaleDB. Estimated from WLED's ABL current (5V rail).
    void publishPower() {
      if (mqtt == nullptr) return;
      double ma = BusManager::currentMilliamps();
      JsonDocument out;
      out["watts"] = ma * 5.0 / 1000.0;
      out["amps"] = ma / 1000.0;
      out["volts"] = 5.0;
      String buf; serializeJson(out, buf);
      mqtt->publish((base + "/power").c_str(), 0, false, buf.c_str());
    }

    // Health heartbeat -> Redis (fleet diagnostics).
    void publishHealth() {
      if (mqtt == nullptr) return;
      JsonDocument out;
      out["rssi"] = WiFi.RSSI();
      out["free_heap"] = (uint32_t) ESP.getFreeHeap();
      out["uptime"] = (uint32_t) (millis() / 1000);
      out["fw_version"] = PIXC_FW_VERSION;
      String buf; serializeJson(out, buf);
      mqtt->publish((base + "/health").c_str(), 0, false, buf.c_str());
    }

    void emitEvent(const char* type, const char* detailJson) {
      if (mqtt == nullptr) return;
      JsonDocument out;
      out["type"] = type;
      out["detail"] = serialized(detailJson);  // raw JSON passthrough
      String buf; serializeJson(out, buf);
      mqtt->publish((base + "/event").c_str(), 0, false, buf.c_str());
    }

    void publishOtaProgress(const char* jobId, const char* status, int percent) {
      if (mqtt == nullptr) return;
      JsonDocument p;
      p["job_id"] = jobId;
      p["status"] = status;
      p["percent"] = percent;
      String buf;
      serializeJson(p, buf);
      mqtt->publish((base + "/ota/progress").c_str(), 0, false, buf.c_str());
    }

    void handleOta(const char* payload) {
      JsonDocument in;
      if (deserializeJson(in, payload)) return;
      String jobId = in["job_id"] | "";
      String url = in["url"] | "";
      if (url.isEmpty()) return;

      // TODO(prod): download in["sig_url"] and verify the Ed25519 signature over the
      // binary against the public key baked into firmware BEFORE flashing. Reject on fail.
      publishOtaProgress(jobId.c_str(), "downloading", 0);

      WiFiClientSecure client;
      client.setInsecure();  // TODO(prod): pin the firmware.pixc.app certificate
      httpUpdate.rebootOnUpdate(true);
      publishOtaProgress(jobId.c_str(), "installing", 50);
      t_httpUpdate_return ret = httpUpdate.update(client, url);
      if (ret == HTTP_UPDATE_FAILED) {
        publishOtaProgress(jobId.c_str(), "failed", 0);
        emitEvent("ota_failed", "{}");
      }
      // On success the device reboots (rebootOnUpdate) and re-announces on reconnect.
    }

    // Device bootstrap: ask the cloud which MQTT broker to use, then point WLED at it.
    void provisionBroker() {
      HTTPClient http;
      String url = apiBase + "/api/v1/provision?mac=" + escapedMac;
      if (!http.begin(url)) return;
      int code = http.GET();
      if (code == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString())) {
          JsonObject d = doc["data"];
          const char* host = d["mqtt_host"] | "";
          int port = d["mqtt_port"] | 1883;
          if (host[0] != 0) {
            strncpy(mqttServer, host, MQTT_MAX_SERVER_LEN);
            mqttServer[MQTT_MAX_SERVER_LEN] = '\0';
            mqttPort = (uint16_t) port;
            mqttEnabled = true;
            initMqtt();              // (re)connect WLED's MQTT client to the provisioned broker
            provisioned = true;
          }
        }
      }
      http.end();
    }

    // Maps PixC effect names to WLED FX indices. Extend as the effect set grows.
    uint8_t effectToFx(const char* name) {
      if (strcmp(name, "solid") == 0)   return FX_MODE_STATIC;
      if (strcmp(name, "breeze") == 0)  return FX_MODE_BREATH;
      if (strcmp(name, "sunrise") == 0) return FX_MODE_SUNRISE;
      if (strcmp(name, "aurora") == 0)  return FX_MODE_AURORA;
      if (strcmp(name, "pulse") == 0)   return FX_MODE_STROBE;
      return FX_MODE_STATIC;
    }

    const char* fxToEffect(uint8_t fx) {
      switch (fx) {
        case FX_MODE_STATIC:  return "solid";
        case FX_MODE_BREATH:  return "breeze";
        case FX_MODE_SUNRISE: return "sunrise";
        case FX_MODE_AURORA:  return "aurora";
        case FX_MODE_STROBE:  return "pulse";
        default:              return "solid";
      }
    }

  public:
    // Persist the PixC API origin in WLED config (Settings -> Usermods, or set by the app during pairing).
    void addToConfig(JsonObject& root) override {
      JsonObject top = root["pixc"].to<JsonObject>();
      top["api_base"] = apiBase;
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root["pixc"];
      if (top.isNull()) return false;
      apiBase = (const char*) (top["api_base"] | "");
      return true;
    }
};
