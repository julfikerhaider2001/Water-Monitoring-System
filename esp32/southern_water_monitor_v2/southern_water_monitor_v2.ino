/*
 * ============================================================
 *  SOUTHERN WATER MONITOR — Firmware v2.0
 *  BLE Configuration — No Hotspot — No Webpage Burden
 *  By: Southern IoT
 * ============================================================
 *
 *  WHAT'S NEW IN V2:
 *   - BLE handles all first-time setup (no hotspot needed)
 *   - webpage.h completely removed (no web server burden)
 *   - Only a tiny JSON API remains for server dashboard
 *   - Flutter app + BLE = full control
 *
 *  HARDWARE:
 *   - ESP32 (single device — sensor wired directly)
 *   - HC-SR04 Ultrasonic sensor
 *   - 1 channel relay module
 *   - DPDT switch (inside box — AUTO/MANUAL)
 *   - External DP switch (outside box — manual motor control)
 *   - 3 LEDs: Status (GPIO2), Auto (GPIO4), Manual (GPIO16)
 *   - HiLink PM01 (230V to 5V)
 *
 *  BLE UUIDs (keep same in Flutter app):
 *   Service:       4e535700-1234-5678-abcd-00000000000a
 *   INFO  (read):  4e535701-1234-5678-abcd-00000000000a
 *   CONFIG(write): 4e535702-1234-5678-abcd-00000000000a
 *   NOTIFY:        4e535703-1234-5678-abcd-00000000000a
 *   STATUS(read):  4e535704-1234-5678-abcd-00000000000a
 *
 *  LIBRARIES NEEDED:
 *   - NimBLE-Arduino    (install from Library Manager)
 *   - ArduinoJson v6.x  (install from Library Manager)
 *
 *  CONFIG WRITE FORMAT (from Flutter app over BLE):
 *   Command:Value
 *   Examples:
 *     AUTH:12345678
 *     WIFI:MySSID|MyPassword
 *     TANK:120|10
 *     LEVEL:20|90
 *     AUTOEN:1
 *     PUMP:1
 *     PUMP:0
 *     CHANGEPASS:oldpass|newpass
 * ============================================================
 */

#include <WiFi.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WebServer.h>   // Lightweight — only for JSON API, no HTML

// ===================== PINS =====================
#define TRIG_PIN      32
#define ECHO_PIN      34
#define RELAY_PIN     27
#define LED_STATUS   13
#define LED_AUTO      25
#define LED_MANUAL    12
#define DPDT_PIN      23   // HIGH = MANUAL, LOW = AUTO (10kΩ pull-down to GND)

// ===================== BLE UUIDs =====================
#define BLE_SERVICE_UUID   "4e535700-1234-5678-abcd-00000000000a"
#define BLE_INFO_UUID      "4e535701-1234-5678-abcd-00000000000a"  // READ
#define BLE_CONFIG_UUID    "4e535702-1234-5678-abcd-00000000000a"  // WRITE
#define BLE_NOTIFY_UUID    "4e535703-1234-5678-abcd-00000000000a"  // NOTIFY
#define BLE_STATUS_UUID    "4e535704-1234-5678-abcd-00000000000a"  // READ (live status)

// ===================== FIRMWARE =====================
#define FW_VERSION    "2.0.0"

// ===================== OBJECTS =====================
Preferences   prefs;
WebServer     apiServer(80);  // tiny API server — JSON only, no HTML

// ===================== CONFIG =====================
struct Config {
  String devicePassword;
  int    tankHeight;
  int    overflowDist;
  int    startLevel;
  int    stopLevel;
  bool   autoEnabled;
  String wifiSSID;
  String wifiPass;
} cfg;

// ===================== RUNTIME =====================
struct Runtime {
  float  distCm;
  float  levelPct;
  bool   pumpRunning;
  bool   manualMode;
  bool   wifiConnected;
  bool   sensorError;
  bool   bleClientConnected;
  bool   bleAuthenticated;    // phone must AUTH before sending config
  unsigned long pumpStartTime;
  unsigned long lastStatusBlink;
  int    statusPhase;
} rt;

// ===================== BLE GLOBALS =====================
NimBLECharacteristic* pNotifyChar   = nullptr;
NimBLECharacteristic* pStatusChar   = nullptr;
NimBLECharacteristic* pInfoChar     = nullptr;

// ===================== NOTIFY QUEUE =====================
// Notifications queued here and sent from main loop
// Avoids calling notify() inside BLE stack callback (causes silent fail)
#define NOTIFY_QUEUE_SIZE 8
String notifyQueue[NOTIFY_QUEUE_SIZE];
int    notifyHead = 0;
int    notifyTail = 0;

void queueNotify(String msg) {
  int next = (notifyTail + 1) % NOTIFY_QUEUE_SIZE;
  if (next != notifyHead) {
    notifyQueue[notifyTail] = msg;
    notifyTail = next;
  }
}

void flushNotifyQueue() {
  while (notifyHead != notifyTail) {
    if (rt.bleClientConnected && pNotifyChar) {
      pNotifyChar->setValue(notifyQueue[notifyHead].c_str());
      pNotifyChar->notify();
      Serial.print("[BLE] Notify: ");
      Serial.println(notifyQueue[notifyHead]);
    }
    notifyHead = (notifyHead + 1) % NOTIFY_QUEUE_SIZE;
  }
}

// ===================== HELPERS =====================
String getDeviceName() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[32];
  sprintf(name, "SouthernWater-%02X%02X", mac[4], mac[5]);
  return String(name);
}

String getMacString() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char s[18];
  sprintf(s, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  return String(s);
}

void bleNotify(String msg) {
  // Queue the message — sent from main loop to avoid BLE stack callback issues
  Serial.print("[BLE] Queuing notify: "); Serial.println(msg);
  queueNotify(msg);
}

// ===================== PREFERENCES =====================
void loadConfig() {
  prefs.begin("swm", true);
  cfg.devicePassword = prefs.getString("devpass", "12345678");
  cfg.tankHeight      = prefs.getInt("tankH",     100);
  cfg.overflowDist    = prefs.getInt("ovfDist",   10);
  cfg.startLevel      = prefs.getInt("startLvl",  20);
  cfg.stopLevel       = prefs.getInt("stopLvl",   90);
  cfg.autoEnabled     = prefs.getBool("autoEn",   true);
  cfg.wifiSSID        = prefs.getString("wSSID",  "");
  cfg.wifiPass        = prefs.getString("wPass",  "");
  prefs.end();
  Serial.println("[CFG] Loaded");
}

void saveConfig() {
  prefs.begin("swm", false);
  prefs.putString("devpass", cfg.devicePassword);
  prefs.putInt("tankH",      cfg.tankHeight);
  prefs.putInt("ovfDist",    cfg.overflowDist);
  prefs.putInt("startLvl",   cfg.startLevel);
  prefs.putInt("stopLvl",    cfg.stopLevel);
  prefs.putBool("autoEn",    cfg.autoEnabled);
  prefs.putString("wSSID",   cfg.wifiSSID);
  prefs.putString("wPass",   cfg.wifiPass);
  prefs.end();
  Serial.println("[CFG] Saved");
}

// ===================== SENSOR =====================
float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long dur = pulseIn(ECHO_PIN, HIGH, 35000);
  if (dur == 0) return -1.0;
  return (dur * 0.034) / 2.0;
}

float calcLevelPct(float distCm) {
  float waterDepth  = cfg.tankHeight - distCm;
  float usableDepth = cfg.tankHeight - cfg.overflowDist;
  if (usableDepth <= 0) return 0;
  return constrain((waterDepth / usableDepth) * 100.0, 0.0, 100.0);
}

// ===================== LED PATTERNS =====================
void handleLEDs() {
  digitalWrite(LED_AUTO,   rt.manualMode ? LOW  : HIGH);
  digitalWrite(LED_MANUAL, rt.manualMode ? HIGH : LOW);

  unsigned long now = millis();

  if (rt.sensorError) {
    // Triple fast blink pause — sensor error
    unsigned long iv[] = {100,100,100,100,100,1500};
    if (now - rt.lastStatusBlink >= iv[rt.statusPhase % 6]) {
      rt.lastStatusBlink = now;
      rt.statusPhase = (rt.statusPhase + 1) % 6;
      digitalWrite(LED_STATUS, (rt.statusPhase % 2 == 0) ? HIGH : LOW);
    }
  } else if (!rt.wifiConnected && cfg.wifiSSID.length() > 0) {
    // WiFi configured but not connected — medium fast blink
    if (now - rt.lastStatusBlink >= 400) {
      digitalWrite(LED_STATUS, !digitalRead(LED_STATUS));
      rt.lastStatusBlink = now;
    }
  } else if (!rt.wifiConnected) {
    // No WiFi configured — very slow blink (waiting for BLE setup)
    if (now - rt.lastStatusBlink >= 2000) {
      digitalWrite(LED_STATUS, !digitalRead(LED_STATUS));
      rt.lastStatusBlink = now;
    }
  } else if (rt.manualMode) {
    // Double pulse — manual mode
    unsigned long iv[] = {120, 120, 120, 900};
    if (now - rt.lastStatusBlink >= iv[rt.statusPhase % 4]) {
      rt.lastStatusBlink = now;
      rt.statusPhase = (rt.statusPhase + 1) % 4;
      bool on = (rt.statusPhase == 0 || rt.statusPhase == 2);
      digitalWrite(LED_STATUS, on ? HIGH : LOW);
    }
  } else if (rt.pumpRunning) {
    // Fast blink — pump running
    if (now - rt.lastStatusBlink >= 150) {
      digitalWrite(LED_STATUS, !digitalRead(LED_STATUS));
      rt.lastStatusBlink = now;
    }
  } else {
    // Slow blink — all OK standby
    if (now - rt.lastStatusBlink >= 1000) {
      digitalWrite(LED_STATUS, !digitalRead(LED_STATUS));
      rt.lastStatusBlink = now;
    }
  }
}

// ===================== PUMP CONTROL =====================
void controlPump() {
  rt.manualMode = (digitalRead(DPDT_PIN) == HIGH);

  if (rt.manualMode) {
    if (rt.pumpRunning) {
      digitalWrite(RELAY_PIN, LOW);
      rt.pumpRunning = false;
    }
    return;
  }

  if (!cfg.autoEnabled || rt.sensorError) {
    digitalWrite(RELAY_PIN, LOW);
    rt.pumpRunning = false;
    return;
  }

  if (!rt.pumpRunning && rt.levelPct <= cfg.startLevel) {
    rt.pumpRunning   = true;
    rt.pumpStartTime = millis();
    digitalWrite(RELAY_PIN, HIGH);
    Serial.printf("[PUMP] ON  — %.1f%% <= %d%%\n", rt.levelPct, cfg.startLevel);
  } else if (rt.pumpRunning && rt.levelPct >= cfg.stopLevel) {
    rt.pumpRunning = false;
    digitalWrite(RELAY_PIN, LOW);
    Serial.printf("[PUMP] OFF — %.1f%% >= %d%%\n", rt.levelPct, cfg.stopLevel);
  }

  // 30 minute overtime protection
  if (rt.pumpRunning && (millis() - rt.pumpStartTime > 1800000UL)) {
    rt.pumpRunning = false;
    digitalWrite(RELAY_PIN, LOW);
    Serial.println("[PUMP] OVERTIME — forced OFF");
    bleNotify("ALERT:pump_overtime");
  }
}

// ===================== WIFI =====================
void connectToWifi() {
  if (cfg.wifiSSID.length() == 0) return;
  Serial.print("[WIFI] Connecting to: "); Serial.println(cfg.wifiSSID);
  WiFi.begin(cfg.wifiSSID.c_str(), cfg.wifiPass.c_str());
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500); tries++; Serial.print(".");
  }
  Serial.println();
  rt.wifiConnected = (WiFi.status() == WL_CONNECTED);
  if (rt.wifiConnected) {
    Serial.print("[WIFI] Connected! IP: "); Serial.println(WiFi.localIP());
    // Update BLE info characteristic with new IP
    if (pInfoChar) {
      String info = buildInfoJson();
      pInfoChar->setValue(info.c_str());
    }
  } else {
    Serial.println("[WIFI] Failed");
  }
}

// ===================== JSON BUILDERS =====================
String buildInfoJson() {
  StaticJsonDocument<256> doc;
  doc["name"]    = getDeviceName();
  doc["mac"]     = getMacString();
  doc["fw"]      = FW_VERSION;
  doc["wifiSet"] = (cfg.wifiSSID.length() > 0);
  doc["wifiConn"]= rt.wifiConnected;
  doc["ip"]      = rt.wifiConnected ? WiFi.localIP().toString() : "";
  String out; serializeJson(doc, out);
  return out;
}

String buildStatusJson() {
  StaticJsonDocument<384> doc;
  doc["level"]      = rt.sensorError ? -1 : round(rt.levelPct * 10) / 10.0;
  doc["dist"]       = rt.sensorError ? -1 : round(rt.distCm * 10) / 10.0;
  doc["pump"]       = rt.pumpRunning;
  doc["mode"]       = rt.manualMode ? "MANUAL" : "AUTO";
  doc["autoEn"]     = cfg.autoEnabled;
  doc["sensorErr"]  = rt.sensorError;
  doc["wifiConn"]   = rt.wifiConnected;
  doc["ip"]         = rt.wifiConnected ? WiFi.localIP().toString() : "";
  doc["tankH"]      = cfg.tankHeight;
  doc["ovfDist"]    = cfg.overflowDist;
  doc["startLvl"]   = cfg.startLevel;
  doc["stopLvl"]    = cfg.stopLevel;
  doc["fw"]         = FW_VERSION;
  doc["mac"]        = getMacString();
  String out; serializeJson(doc, out);
  return out;
}

// ===================== BLE CONFIG PARSER =====================
/*
 * All configuration from Flutter app arrives as:
 * COMMAND:value
 *
 * Commands:
 *  AUTH:password              → authenticate session
 *  WIFI:ssid|password         → save wifi and connect
 *  TANK:height|overflowDist   → save tank dimensions
 *  LEVEL:startPct|stopPct     → save level thresholds
 *  AUTOEN:1 or 0              → enable/disable auto mode
 *  PUMP:1 or 0                → manual pump override (auto mode only)
 *  CHANGEPASS:old|new         → change device password
 *  STATUS                     → request status (replied via notify)
 *  SCANWIFI                   → not needed via BLE (app uses phone WiFi scan)
 */
void processBLECommand(String data) {
  data.trim();
  Serial.print("[BLE] CMD: "); Serial.println(data);

  int sep = data.indexOf(':');
  if (sep == -1) { bleNotify("ERR:bad_format"); return; }

  String cmd = data.substring(0, sep);
  String val = data.substring(sep + 1);
  cmd.toUpperCase();

  // ---- AUTH (must do first) ----
  if (cmd == "AUTH") {
    if (val == cfg.devicePassword) {
      rt.bleAuthenticated = true;
      bleNotify("AUTH:ok");
      Serial.println("[BLE] Authenticated");
    } else {
      rt.bleAuthenticated = false;
      bleNotify("AUTH:fail");
      Serial.println("[BLE] Auth failed");
    }
    return;
  }

  // ---- All other commands require auth ----
  if (!rt.bleAuthenticated) {
    bleNotify("ERR:not_authenticated");
    return;
  }

  // ---- WIFI ----
  if (cmd == "WIFI") {
    int s = val.indexOf('|');
    if (s == -1) { bleNotify("ERR:bad_wifi_format"); return; }
    cfg.wifiSSID = val.substring(0, s);
    cfg.wifiPass = val.substring(s + 1);
    saveConfig();
    bleNotify("WIFI:connecting");
    connectToWifi();
    if (rt.wifiConnected) {
      setupApiServer();   // start HTTP server immediately after first WiFi connect
      bleNotify("WIFI:ok:" + WiFi.localIP().toString());
    } else {
      bleNotify("WIFI:fail");
    }
    return;
  }

  // ---- TANK dimensions ----
  if (cmd == "TANK") {
    int s = val.indexOf('|');
    if (s == -1) { bleNotify("ERR:bad_tank_format"); return; }
    int h = val.substring(0, s).toInt();
    int o = val.substring(s + 1).toInt();
    if (h < 10 || h > 500) { bleNotify("ERR:invalid_height"); return; }
    cfg.tankHeight   = h;
    cfg.overflowDist = o;
    saveConfig();
    bleNotify("TANK:ok");
    Serial.printf("[CFG] Tank: %dcm, Overflow: %dcm\n", h, o);
    return;
  }

  // ---- LEVEL thresholds ----
  if (cmd == "LEVEL") {
    int s = val.indexOf('|');
    if (s == -1) { bleNotify("ERR:bad_level_format"); return; }
    int start = val.substring(0, s).toInt();
    int stop  = val.substring(s + 1).toInt();
    if (start >= stop) { bleNotify("ERR:start_must_be_less_than_stop"); return; }
    cfg.startLevel = start;
    cfg.stopLevel  = stop;
    saveConfig();
    bleNotify("LEVEL:ok");
    Serial.printf("[CFG] Level: start=%d%%, stop=%d%%\n", start, stop);
    return;
  }

  // ---- AUTO ENABLE ----
  if (cmd == "AUTOEN") {
    cfg.autoEnabled = (val == "1");
    saveConfig();
    bleNotify(cfg.autoEnabled ? "AUTOEN:1" : "AUTOEN:0");
    if (!cfg.autoEnabled) {
      digitalWrite(RELAY_PIN, LOW);
      rt.pumpRunning = false;
    }
    return;
  }

  // ---- PUMP OVERRIDE ----
  if (cmd == "PUMP") {
    if (rt.manualMode) {
      bleNotify("ERR:device_in_manual_mode");
      return;
    }
    bool on = (val == "1");
    rt.pumpRunning = on;
    digitalWrite(RELAY_PIN, on ? HIGH : LOW);
    if (on) rt.pumpStartTime = millis();
    bleNotify(on ? "PUMP:on" : "PUMP:off");
    Serial.printf("[PUMP] App override: %s\n", on ? "ON" : "OFF");
    return;
  }

  // ---- CHANGE PASSWORD ----
  if (cmd == "CHANGEPASS") {
    int s = val.indexOf('|');
    if (s == -1) { bleNotify("ERR:bad_pass_format"); return; }
    String oldp = val.substring(0, s);
    String newp = val.substring(s + 1);
    if (oldp != cfg.devicePassword) { bleNotify("ERR:wrong_old_password"); return; }
    if (newp.length() < 6) { bleNotify("ERR:password_too_short"); return; }
    cfg.devicePassword = newp;
    saveConfig();
    bleNotify("CHANGEPASS:ok");
    return;
  }

  // ---- STATUS REQUEST ----
  if (cmd == "STATUS") {
    bleNotify("STATUS:" + buildStatusJson());
    return;
  }

  bleNotify("ERR:unknown_command");
}

// ===================== BLE CALLBACKS =====================
class BLEServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    rt.bleClientConnected = true;
    rt.bleAuthenticated   = false;
    Serial.println("[BLE] Phone connected");
    bleNotify("CONNECTED:" + getDeviceName() +
              ":wifiSet=" + (cfg.wifiSSID.length() > 0 ? "1" : "0"));
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    rt.bleClientConnected = false;
    rt.bleAuthenticated   = false;
    Serial.println("[BLE] Phone disconnected — restarting advertising");
    NimBLEDevice::getAdvertising()->start();
  }
};

class ConfigWriteCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    String data = String(pChar->getValue().c_str());
    Serial.print("[BLE] onWrite: "); Serial.println(data);
    processBLECommand(data);
  }
};

// ===================== BLE SETUP =====================
void setupBLE() {
  String devName = getDeviceName();
  NimBLEDevice::init(devName.c_str());
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); // max BLE power

  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new BLEServerCB());

  NimBLEService* pService = pServer->createService(BLE_SERVICE_UUID);

  // INFO characteristic — read device info (no auth needed)
  pInfoChar = pService->createCharacteristic(
    BLE_INFO_UUID, NIMBLE_PROPERTY::READ
  );
  pInfoChar->setValue(buildInfoJson().c_str());

  // CONFIG characteristic — write commands (auth required)
  NimBLECharacteristic* pConfigChar = pService->createCharacteristic(
    BLE_CONFIG_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pConfigChar->setCallbacks(new ConfigWriteCB());

  // NOTIFY characteristic — device pushes results to phone
  pNotifyChar = pService->createCharacteristic(
    BLE_NOTIFY_UUID, NIMBLE_PROPERTY::NOTIFY
  );

  // STATUS characteristic — read live status (auth required to be useful)
  pStatusChar = pService->createCharacteristic(
    BLE_STATUS_UUID, NIMBLE_PROPERTY::READ
  );
  pStatusChar->setValue(buildStatusJson().c_str());

  pService->start();

  // Advertising — include service UUID so app can filter
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(BLE_SERVICE_UUID);
  pAdv->setScanResponseData(NimBLEAdvertisementData());
  pAdv->start();

  Serial.print("[BLE] Advertising as: "); Serial.println(devName);
}

// ===================== MINIMAL API SERVER =====================
// Only JSON — no HTML — no burden
void setupApiServer() {
  // Status endpoint — for your server dashboard to poll
  apiServer.on("/api/status", HTTP_GET, []() {
    apiServer.sendHeader("Access-Control-Allow-Origin", "*");
    apiServer.send(200, "application/json", buildStatusJson());
  });

  // Pump control — for your server dashboard
  apiServer.on("/api/pump", HTTP_POST, []() {
    if (!apiServer.hasArg("plain")) {
      apiServer.send(400, "application/json", "{\"ok\":false}");
      return;
    }
    StaticJsonDocument<64> doc;
    deserializeJson(doc, apiServer.arg("plain"));
    bool on = doc["on"] | false;
    if (!rt.manualMode) {
      rt.pumpRunning = on;
      digitalWrite(RELAY_PIN, on ? HIGH : LOW);
      if (on) rt.pumpStartTime = millis();
    }
    apiServer.send(200, "application/json",
      rt.manualMode ? "{\"ok\":false,\"msg\":\"manual_mode\"}" : "{\"ok\":true}");
  });

  // Settings endpoint — for your server dashboard
  apiServer.on("/api/settings", HTTP_POST, []() {
    StaticJsonDocument<256> doc;
    deserializeJson(doc, apiServer.arg("plain"));
    if (doc.containsKey("tankH"))    cfg.tankHeight   = doc["tankH"];
    if (doc.containsKey("ovfDist"))  cfg.overflowDist = doc["ovfDist"];
    if (doc.containsKey("startLvl")) cfg.startLevel   = doc["startLvl"];
    if (doc.containsKey("stopLvl"))  cfg.stopLevel    = doc["stopLvl"];
    if (doc.containsKey("autoEn"))   cfg.autoEnabled  = doc["autoEn"];
    saveConfig();
    apiServer.send(200, "application/json", "{\"ok\":true}");
  });

  apiServer.begin();
  Serial.println("[API] Minimal JSON server started on port 80");
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=============================");
  Serial.println("  Southern Water Monitor");
  Serial.println("  Firmware v" FW_VERSION);
  Serial.println("=============================");

  pinMode(TRIG_PIN,   OUTPUT);
  pinMode(ECHO_PIN,   INPUT);
  pinMode(RELAY_PIN,  OUTPUT);
  pinMode(LED_STATUS, OUTPUT);
  pinMode(LED_AUTO,   OUTPUT);
  pinMode(LED_MANUAL, OUTPUT);
  pinMode(DPDT_PIN,   INPUT);

  digitalWrite(RELAY_PIN,  LOW);
  digitalWrite(LED_STATUS, LOW);
  digitalWrite(LED_AUTO,   LOW);
  digitalWrite(LED_MANUAL, LOW);

  rt = {};
  loadConfig();

  // BLE always starts — this is how phone discovers and configures device
  setupBLE();

  // WiFi — only if credentials saved
  if (cfg.wifiSSID.length() > 0) {
    WiFi.mode(WIFI_STA);
    connectToWifi();
    if (rt.wifiConnected) setupApiServer();
  } else {
    Serial.println("[WIFI] No credentials — waiting for BLE config");
  }

  // Startup blink
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_STATUS, HIGH); delay(80);
    digitalWrite(LED_STATUS, LOW);  delay(80);
  }

  Serial.println("[SYS] Ready");
  Serial.print("[BLE] Device name: "); Serial.println(getDeviceName());
  Serial.print("[BLE] MAC: "); Serial.println(getMacString());
}

// ===================== LOOP =====================
unsigned long lastSensorRead  = 0;
unsigned long lastStatusUpdate = 0;
unsigned long lastWifiCheck   = 0;

void loop() {
  // Sensor — every 2 seconds
  if (millis() - lastSensorRead >= 2000) {
    lastSensorRead = millis();
    rt.distCm     = readDistanceCm();
    rt.sensorError = (rt.distCm < 0 || rt.distCm > (cfg.tankHeight + 30));
    if (!rt.sensorError) {
      rt.levelPct = calcLevelPct(rt.distCm);
    }
  }

  // Pump control
  controlPump();

  // Update BLE status characteristic every 5 seconds
  if (millis() - lastStatusUpdate >= 5000) {
    lastStatusUpdate = millis();
    if (pStatusChar) pStatusChar->setValue(buildStatusJson().c_str());
  }

  // WiFi check every 15 seconds
  if (millis() - lastWifiCheck >= 15000) {
    lastWifiCheck = millis();
    bool wasConnected = rt.wifiConnected;
    rt.wifiConnected = (WiFi.status() == WL_CONNECTED);
    // Reconnect if dropped
    if (!rt.wifiConnected && cfg.wifiSSID.length() > 0) {
      Serial.println("[WIFI] Dropped — reconnecting...");
      WiFi.reconnect();
    }
    // Start API server if just connected
    if (!wasConnected && rt.wifiConnected) {
      setupApiServer();
    }
  }

  // Handle API requests (only runs if WiFi connected)
  if (rt.wifiConnected) apiServer.handleClient();

  // LEDs
  handleLEDs();

  // Flush BLE notify queue — send queued notifications from main loop
  flushNotifyQueue();
}
