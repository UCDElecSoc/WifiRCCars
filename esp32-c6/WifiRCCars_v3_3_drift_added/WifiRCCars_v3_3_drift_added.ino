#include <WiFi.h>
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include <Adafruit_NeoPixel.h>
#include "../WiFi_Credentials.h"

using namespace websockets;

// ====== User Config ======
const char* WS_URL = "ws://192.168.50.51:8000/ws";

constexpr int NEOPIXEL_PIN = 8;
constexpr int NEOPIXEL_COUNT = 1;

// Motor pins (verified)
// IN1 (GPIO11): Left motor reverse
// IN2 (GPIO10): Left motor forward
// IN3 (GPIO20): Right motor reverse
// IN4 (GPIO21): Right motor forward
constexpr int IN1 = 11;
constexpr int IN2 = 10;
constexpr int IN3 = 20;
constexpr int IN4 = 21;

// PWM config (Arduino-style 0-255)

constexpr unsigned long TELEMETRY_INTERVAL_MS = 3000;
constexpr unsigned long WIFI_RETRY_MS = 2000;
constexpr unsigned long WS_RETRY_MS = 2000;
constexpr unsigned long COMMAND_TIMEOUT_MS = 350; // failsafe window
constexpr unsigned long STATUS_PRINT_MS = 200;
constexpr unsigned long LED_FLASH_CONNECTED_MS = 800;
constexpr unsigned long LED_FLASH_DISCONNECTED_MS = 150;

// ====== State ======
WebsocketsClient ws;
Adafruit_NeoPixel pixels(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

String targetId;
String ipString;
String statusText = "booting";

float leftSpeed = 0.0f;   // expected -255..255 (from bindings)
float rightSpeed = 0.0f;  // expected -255..255 (from bindings)

unsigned long lastTelemetryMs = 0;
unsigned long lastWifiAttemptMs = 0;
unsigned long lastWsAttemptMs = 0;
unsigned long lastCommandMs = 0;
unsigned long lastStatusPrintMs = 0;
unsigned long lastLedToggleMs = 0;
bool wsConnected = false;
bool ledOn = false;

// ====== Helpers ======
void setStatus(const char* message) {
  statusText = message;
}

String buildTargetIdFromIp(IPAddress ip) {
  int last = ip[3];
  int suffix = last % 100;
  if (suffix < 0) suffix = 0;
  if (suffix > 99) suffix = 99;
  char buf[8];
  snprintf(buf, sizeof(buf), "esp-%02d", suffix);
  return String(buf);
}

void applyLedColor(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t color = pixels.Color(r, g, b);
  pixels.setPixelColor(0, color);
  pixels.show();
}

uint8_t clampPwm(float value) {
  int v = (int)roundf(fabs(value));
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  return (uint8_t)v;
}

void applyMotorSidePwm(float speed, int pinForward, int pinReverse) {
  if (speed > 0.0f) {
    analogWrite(pinForward, clampPwm(speed));
    analogWrite(pinReverse, 0);
  } else if (speed < 0.0f) {
    analogWrite(pinForward, 0);
    analogWrite(pinReverse, clampPwm(speed));
  } else {
    analogWrite(pinForward, 0);
    analogWrite(pinReverse, 0);
  }
}

void applyMotors() {
  // Left motor: forward IN2, reverse IN1
  applyMotorSidePwm(leftSpeed, IN2, IN1);
  // Right motor: forward IN4, reverse IN3
  applyMotorSidePwm(rightSpeed, IN4, IN3);
}

void applySafeOutputs() {
  leftSpeed = 0.0f;
  rightSpeed = 0.0f;
  applyMotors();
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
      if (values.containsKey("left_speed")) leftSpeed = values["left_speed"];
      if (values.containsKey("right_speed")) rightSpeed = values["right_speed"];

      lastCommandMs = millis();
      setStatus("running");
      applyMotors();
    }
  } else if (strcmp(type, "target_registered") == 0) {
    setStatus("registered");
  }
}

void setupWifi() {
  setStatus("wifi-connecting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastWifiAttemptMs < WIFI_RETRY_MS) return;
  lastWifiAttemptMs = now;
  setStatus("wifi-connecting");
  WiFi.disconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void connectWebSocket() {
  setStatus("ws-connecting");

  ws.onMessage([](WebsocketsMessage msg) {
    handleMessage(msg.data());
  });

  ws.onEvent([](WebsocketsEvent event, String data) {
    if (event == WebsocketsEvent::ConnectionOpened) {
      wsConnected = true;
      setStatus("ws-connected");
      sendRegister();
    } else if (event == WebsocketsEvent::ConnectionClosed) {
      wsConnected = false;
      setStatus("ws-lost");
      applySafeOutputs();
    }
  });

  ws.connect(WS_URL);
}

void ensureWebSocket() {
  if (WiFi.status() != WL_CONNECTED) {
    setStatus("wifi-lost");
    wsConnected = false;
    return;
  }
  if (ws.available()) return;

  unsigned long now = millis();
  if (now - lastWsAttemptMs < WS_RETRY_MS) return;
  lastWsAttemptMs = now;
  connectWebSocket();
}

void checkFailsafe() {
  unsigned long now = millis();
  if (wsConnected && (now - lastCommandMs > COMMAND_TIMEOUT_MS)) {
    setStatus("failsafe");
    applySafeOutputs();
  }
}

void updateStatusLed() {
  unsigned long now = millis();
  bool connected = (WiFi.status() == WL_CONNECTED) && wsConnected;
  unsigned long interval = connected ? LED_FLASH_CONNECTED_MS : LED_FLASH_DISCONNECTED_MS;
  if (now - lastLedToggleMs < interval) return;
  lastLedToggleMs = now;
  ledOn = !ledOn;
  if (connected) {
    if (ledOn) {
      applyLedColor(0, 0, 80);
    } else {
      applyLedColor(0, 0, 0);
    }
  } else {
    if (ledOn) {
      applyLedColor(80, 0, 0);
    } else {
      applyLedColor(0, 0, 0);
    }
  }
}

void printStatus() {
  unsigned long now = millis();
  if (now - lastStatusPrintMs < STATUS_PRINT_MS) return;
  lastStatusPrintMs = now;

  Serial.print("[");
  Serial.print(now);
  Serial.print("] [status] ");
  Serial.print(statusText);
  Serial.print(" | wifi=");
  Serial.print(WiFi.status() == WL_CONNECTED ? "ok" : "lost");
  Serial.print(" rssi=");
  Serial.print(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  Serial.print(" | ws=");
  Serial.print(wsConnected ? "ok" : "lost");
  Serial.print(" | left=");
  Serial.print(leftSpeed);
  Serial.print(" right=");
  Serial.print(rightSpeed);
  Serial.print(" | led=");
  Serial.print((WiFi.status() == WL_CONNECTED) && wsConnected ? "blue" : "red");
  Serial.print(" | ip=");
  Serial.println(ipString);
}

void setup() {
  Serial.begin(115200);

  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  pixels.begin();
  pixels.clear();
  pixels.show();

  applySafeOutputs();
  setupWifi();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED && ipString.length() == 0) {
    setStatus("wifi-connected");
    updateIdentityFromWifi();
  }

  ensureWifi();
  ensureWebSocket();

  if (WiFi.status() != WL_CONNECTED) {
    applySafeOutputs();
  }

  ws.poll();

  unsigned long now = millis();
  if (ws.available() && now - lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryMs = now;
    sendTelemetry();
  }

  checkFailsafe();
  updateStatusLed();
  printStatus();
}
