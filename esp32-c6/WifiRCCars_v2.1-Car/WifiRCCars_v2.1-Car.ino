#include <WiFi.h>
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include <Adafruit_NeoPixel.h>
#include "../WiFi_Credentials.h"

using namespace websockets;

// ====== User Config ======
const char* WS_URL = "ws://192.168.50.50:8000/ws";

constexpr int NEOPIXEL_PIN = 8;
constexpr int NEOPIXEL_COUNT = 1;

constexpr int IN1 = 0;
constexpr int IN2 = 1;
constexpr int IN3 = 8;
constexpr int IN4 = 10;

constexpr unsigned long TELEMETRY_INTERVAL_MS = 3000;
constexpr unsigned long WIFI_RETRY_MS = 2000;
constexpr unsigned long WS_RETRY_MS = 2000;

// ====== State ======
WebsocketsClient ws;
Adafruit_NeoPixel pixels(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

String targetId;
String ipString;
String statusText = "booting";

float rVal = 0.0f;
float gVal = 0.0f;
float bVal = 0.0f;
float leftSpeed = 0.0f;
float rightSpeed = 0.0f;

unsigned long lastTelemetryMs = 0;
unsigned long lastWifiAttemptMs = 0;
unsigned long lastWsAttemptMs = 0;

// ====== Helpers ======
String buildTargetIdFromIp(IPAddress ip) {
  int last = ip[3];
  int suffix = last % 100;
  if (suffix < 0) suffix = 0;
  if (suffix > 99) suffix = 99;
  char buf[8];
  snprintf(buf, sizeof(buf), "esp-%02d", suffix);
  return String(buf);
}

void applyLed() {
  int r = constrain((int)rVal, 0, 255);
  int g = constrain((int)gVal, 0, 255);
  int b = constrain((int)bVal, 0, 255);
  uint32_t color = pixels.Color(r, g, b);
  pixels.setPixelColor(0, color);
  pixels.show();
}

void applyMotorSide(float speed, int pinForward, int pinReverse) {
  if (speed > 0.0f) {
    digitalWrite(pinForward, HIGH);
    digitalWrite(pinReverse, LOW);
  } else if (speed < 0.0f) {
    digitalWrite(pinForward, LOW);
    digitalWrite(pinReverse, HIGH);
  } else {
    digitalWrite(pinForward, LOW);
    digitalWrite(pinReverse, LOW);
  }
}

void applyMotors() {
  applyMotorSide(leftSpeed, IN1, IN2);
  applyMotorSide(rightSpeed, IN3, IN4);
}

void updateIdentityFromWifi() {
  IPAddress ip = WiFi.localIP();
  ipString = ip.toString();
  targetId = buildTargetIdFromIp(ip);
}

void sendRegister() {
  StaticJsonDocument<768> doc;
  doc["type"] = "target_register";
  doc["target_id"] = targetId;
  doc["display_name"] = targetId;
  doc["mode"] = "write_only";

  JsonArray vars = doc.createNestedArray("variables");

  auto addVar = [&](const char* name, const char* type, const char* access) {
    JsonObject v = vars.createNestedObject();
    v["name"] = name;
    v["type"] = type;
    v["access"] = access;
  };

  addVar("r", "float", "write");
  addVar("g", "float", "write");
  addVar("b", "float", "write");
  addVar("left_speed", "float", "write");
  addVar("right_speed", "float", "write");

  addVar("ip", "string", "read");
  addVar("status_text", "string", "read");
  addVar("wifi_rssi", "float", "read");

  JsonObject init = doc.createNestedObject("initial_state");
  init["ip"] = ipString;
  init["status_text"] = statusText;
  init["wifi_rssi"] = (float)WiFi.RSSI();

  String payload;
  serializeJson(doc, payload);
  ws.send(payload);
}

void sendTelemetry() {
  StaticJsonDocument<256> doc;
  doc["type"] = "target_telemetry";
  doc["target_id"] = targetId;

  JsonObject values = doc.createNestedObject("values");
  values["ip"] = ipString;
  values["status_text"] = statusText;
  values["wifi_rssi"] = (float)WiFi.RSSI();

  String payload;
  serializeJson(doc, payload);
  ws.send(payload);
}

void handleMessage(const String& message) {
  StaticJsonDocument<768> doc;
  DeserializationError err = deserializeJson(doc, message);
  if (err) return;

  const char* type = doc["type"] | "";
  if (strcmp(type, "target_write_update") == 0) {
    const char* msgTarget = doc["target_id"] | "";
    if (targetId != msgTarget) return;

    JsonObject values = doc["values"];
    if (!values.isNull()) {
      if (values.containsKey("r")) rVal = values["r"];
      if (values.containsKey("g")) gVal = values["g"];
      if (values.containsKey("b")) bVal = values["b"];
      if (values.containsKey("left_speed")) leftSpeed = values["left_speed"];
      if (values.containsKey("right_speed")) rightSpeed = values["right_speed"];

      statusText = "running";
      applyLed();
      applyMotors();
    }
  } else if (strcmp(type, "target_registered") == 0) {
    statusText = "registered";
  }
}

void setupWifi() {
  statusText = "wifi-connecting";
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }
  unsigned long now = millis();
  if (now - lastWifiAttemptMs < WIFI_RETRY_MS) return;
  lastWifiAttemptMs = now;
  statusText = "wifi-connecting";
  WiFi.disconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void connectWebSocket() {
  statusText = "ws-connecting";

  ws.onMessage([](WebsocketsMessage msg) {
    handleMessage(msg.data());
  });

  ws.onEvent([](WebsocketsEvent event, String data) {
    if (event == WebsocketsEvent::ConnectionOpened) {
      statusText = "ws-connected";
      sendRegister();
    } else if (event == WebsocketsEvent::ConnectionClosed) {
      statusText = "ws-lost";
    }
  });

  ws.connect(WS_URL);
}

void ensureWebSocket() {
  if (WiFi.status() != WL_CONNECTED) {
    statusText = "wifi-lost";
    return;
  }
  if (ws.available()) return;

  unsigned long now = millis();
  if (now - lastWsAttemptMs < WS_RETRY_MS) return;
  lastWsAttemptMs = now;
  connectWebSocket();
}

void setup() {
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  pixels.begin();
  pixels.clear();
  pixels.show();

  setupWifi();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED && ipString.length() == 0) {
    statusText = "wifi-connected";
    updateIdentityFromWifi();
  }

  ensureWifi();
  ensureWebSocket();

  ws.poll();

  unsigned long now = millis();
  if (ws.available() && now - lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryMs = now;
    sendTelemetry();
  }
}
