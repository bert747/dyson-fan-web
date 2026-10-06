// Web control for the Dyson purifier. Serves the control page and relays a fixed set of
// commands to the fan's own ESP32 (Dyson_UART_parser firmware), which has no CORS headers
// and so can't be called from the browser directly. Mirrors ../app.py.
//
// Onboarding: if the saved Wi-Fi can't be joined, the board opens a "Dyson-Fan-Setup" hotspot
// with a captive portal to pick a network. Holding BOOT for 5 s forgets Wi-Fi and reopens it.
// The fan controller's address is remembered, and re-found by scanning the subnet if it moves.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "page.h"

static const char *HOSTNAME = "dyson-fan";
static const char *SETUP_AP = "Dyson-Fan-Setup";
static const char *SETUP_PASS = "dysonsetup";
static const char *FAN_NAME = "Dyson ESP Purifier";
static const uint8_t BOOT_PIN = 9;

WebServer server(80);
Preferences prefs;
String fanIp;

// Fan discovery: after a few failed requests, probe one host per loop() pass so the
// page stays responsive while searching.
static uint8_t fanFailures = 0;
static int scanHost = 0;            // 0 = not scanning, else next host number to probe
static uint32_t nextScanAt = 0;

// GET or POST to the fan; returns the HTTP status (negative on connection failure).
static int fanRequest(const String &ip, const char *path, const String &payload, String &body,
                      uint16_t connectTimeout = 4000) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(connectTimeout);
  http.setTimeout(5000);
  if (!http.begin(client, "http://" + ip + path)) return -1;
  int code;
  if (payload.length()) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(payload);
  } else {
    code = http.GET();
  }
  if (code > 0) body = http.getString();
  http.end();
  return code;
}

static int fan(const char *path, const String &payload, String &body) {
  if (scanHost) return -2;  // still searching
  int code = fanRequest(fanIp, path, payload, body);
  if (code < 0 && ++fanFailures >= 3 && !scanHost) {
    Serial.printf("Fan not answering at %s, searching the network\n", fanIp.c_str());
    scanHost = 1;
  }
  if (code > 0) fanFailures = 0;
  return code;
}

static String fanError(int code) {
  return code == -2 ? "searching for the fan" : "fan unreachable (" + String(code) + ")";
}

static void scanStep() {
  if (!scanHost || millis() < nextScanAt) return;
  IPAddress me = WiFi.localIP(), mask = WiFi.subnetMask();
  // Only /24 and smaller subnets are scanned; that covers home networks.
  if (mask[2] != 255) mask = IPAddress(255, 255, 255, 0);
  IPAddress ip(me[0] & mask[0], me[1] & mask[1], me[2] & mask[2], scanHost);
  if (ip != me) {
    String body;
    if (fanRequest(ip.toString(), "/api/states/sensors", "", body, 300) == 200 && body.indexOf(FAN_NAME) >= 0) {
      fanIp = ip.toString();
      prefs.putString("fan_ip", fanIp);
      Serial.printf("Found the fan at %s\n", fanIp.c_str());
      scanHost = 0;
      fanFailures = 0;
      return;
    }
  }
  if (++scanHost > 254) {
    Serial.println("Fan not found, trying again in a minute");
    scanHost = 1;
    nextScanAt = millis() + 60000;
  }
}

static void sendError(int code, const String &msg) {
  JsonDocument doc;
  doc["error"] = msg;
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

static void handleState() {
  String body;
  int code = fan("/api/states/sensors", "", body);
  if (code != 200) return sendError(502, fanError(code));
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

// Sends one {"attributes": name} press; returns an error string, empty on success.
static String press(const char *name) {
  String body;
  int code = fan("/api/controls", String("{\"attributes\":\"") + name + "\"}", body);
  return code == 200 ? "" : fanError(code);
}

static String runCommand(JsonDocument &cmd) {
  String action = cmd["action"] | "";
  if (action == "on" || action == "off") {
    // The firmware only has an on/off toggle, so check first and never flip the wrong way.
    String body;
    int code = fan("/api/states/sensors", "", body);
    if (code != 200) return fanError(code);
    JsonDocument st;
    if (deserializeJson(st, body)) return "bad state from fan";
    bool isOn = st["state"].as<int>() != 0;
    if (isOn != (action == "on")) return press("on_off");
    return "";
  }
  if (action == "speed") {
    if (!cmd["level"].is<int>()) return "speed must be 1-10";
    int level = cmd["level"];
    if (level < 1 || level > 10) return "speed must be 1-10";
    String body;
    int code = fan("/api/controls", "{\"set_fan\":" + String(level) + "}", body);
    return code == 200 ? "" : fanError(code);
  }
  static const char *ATTRIBUTES[] = {"auto_mode_on", "auto_mode_off", "night_mode", "sw_rotation", "direction"};
  for (const char *a : ATTRIBUTES) {
    if (action == a) return press(a);
  }
  return "unknown action";
}

static void handleCmd() {
  JsonDocument cmd;
  if (deserializeJson(cmd, server.arg("plain"))) return sendError(400, "bad json");
  String err = runCommand(cmd);
  if (err.length()) return sendError(err.startsWith("fan") || err.startsWith("searching") ? 502 : 400, err);
  Serial.printf("cmd: %s\n", server.arg("plain").c_str());
  server.send(200, "application/json", "{\"ok\":true}");
}

static void connectWiFi() {
  WiFiManager wm;
  WiFiManagerParameter fanParam("fan_ip", "Fan controller IP (leave blank to find it automatically)", fanIp.c_str(), 40);
  wm.addParameter(&fanParam);
  wm.setSaveParamsCallback([&]() {
    fanIp = fanParam.getValue();
    fanIp.trim();
    prefs.putString("fan_ip", fanIp);
  });
  wm.setTitle("Dyson Fan Setup");
  wm.setHostname(HOSTNAME);
  wm.setConnectTimeout(20);
  // The C3 sometimes misses its first join, so retry the saved network before falling back to setup.
  wm.setConnectRetries(3);
  // If nobody finishes setup (e.g. the router was just rebooting), restart and retry the saved network.
  wm.setConfigPortalTimeout(300);
  Serial.println("Connecting to Wi-Fi (setup hotspot opens if this fails)");
  if (!wm.autoConnect(SETUP_AP, SETUP_PASS)) {
    Serial.println("Setup timed out, restarting");
    ESP.restart();
  }
  Serial.printf("Connected. Open http://%s.local/ or http://%s/\n", HOSTNAME, WiFi.localIP().toString().c_str());
}

static void checkResetButton() {
  static uint32_t pressedAt = 0;
  if (digitalRead(BOOT_PIN) == HIGH) {
    pressedAt = 0;
    return;
  }
  if (!pressedAt) pressedAt = millis();
  if (millis() - pressedAt > 5000) {
    Serial.println("BOOT held: forgetting Wi-Fi and opening setup hotspot");
    WiFiManager wm;
    wm.resetSettings();
    ESP.restart();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(BOOT_PIN, INPUT_PULLUP);
  prefs.begin("dysonweb");
  fanIp = prefs.getString("fan_ip", "10.0.44.168");
  connectWiFi();
  if (fanIp.isEmpty()) scanHost = 1;
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", PAGE_HTML); });
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/cmd", HTTP_POST, handleCmd);
  server.onNotFound([]() { sendError(404, "not found"); });
  server.begin();
}

void loop() {
  server.handleClient();
  scanStep();
  checkResetButton();
}
