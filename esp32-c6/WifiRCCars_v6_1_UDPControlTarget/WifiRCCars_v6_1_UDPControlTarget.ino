#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include <Adafruit_NeoPixel.h>
#include "../WiFi_Credentials.h"

using namespace websockets;

// ====== User Config ======
const char* SERVER_HOST = "192.168.50.51";
const char* WS_URL = "ws://192.168.50.51:8000/ws";

constexpr uint16_t UDP_PORT = 4210;
constexpr uint16_t UDP_MAGIC = 0xCAFE;
constexpr uint8_t UDP_VERSION = 1;
constexpr uint8_t EXPECTED_VAR_COUNT = 5;

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

constexpr unsigned long TELEMETRY_INTERVAL_MS = 250;
constexpr unsigned long WIFI_RETRY_MS = 2000;
constexpr unsigned long WS_RETRY_MS = 2000;
constexpr unsigned long FAILSAFE_MS = 300;
constexpr unsigned long LED_REFRESH_MS = 120;
constexpr unsigned long STATUS_PRINT_MS = 250;

// ====== State ======
WebsocketsClient ws;
WiFiUDP udp;
Adafruit_NeoPixel pixels(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

IPAddress serverIp;
bool serverIpValid = false;

String targetId;
String ipString;
String statusText = "booting";

int16_t leftSpeed = 0;
int16_t rightSpeed = 0;
uint8_t ledR = 0;
uint8_t ledG = 0;
uint8_t ledB = 0;

uint32_t espToken = 0;
bool hasToken = false;

unsigned long lastTelemetryMs = 0;
unsigned long lastWifiAttemptMs = 0;
unsigned long lastWsAttemptMs = 0;
unsigned long lastPacketMs = 0;
unsigned long lastLedMs = 0;
unsigned long lastStatusPrintMs = 0;
bool wsConnected = false;
bool failsafeActive = true;
bool ledOn = false;
bool hasSeq = false;
uint32_t lastSeq = 0;

// ====== Helpers ======
void setStatus(const char* message) {
  statusText = message;
}

void logLine(const char* message) {
  Serial.print("[");
  Serial.print(millis());
  Serial.print("] ");
  Serial.println(message);
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
  pixels.setPixelColor(0, pixels.Color(r, g, b));
  pixels.show();
}

void applyMotorSide(int16_t speed, int pinForward, int pinReverse) {
  // TODO: Replace with proper PWM control for your motor driver.
  if (speed > 0) {
    digitalWrite(pinForward, HIGH);
    digitalWrite(pinReverse, LOW);
  } else if (speed < 0) {
    digitalWrite(pinForward, LOW);
    digitalWrite(pinReverse, HIGH);
  } else {
    digitalWrite(pinForward, LOW);
    digitalWrite(pinReverse, LOW);
  }
}

void applyOutputs() {
  applyMotorSide(leftSpeed, IN2, IN1);
  applyMotorSide(rightSpeed, IN4, IN3);
  applyLedColor(ledR, ledG, ledB);
}

void zeroOutputs() {
  leftSpeed = 0;
  rightSpeed = 0;
  ledR = 0;
  ledG = 0;
  ledB = 0;
  applyOutputs();
}

int16_t clampSpeed(int16_t value) {
  if (value > 255) return 255;
  if (value < -255) return -255;
  return value;
}

uint8_t clampColor(int16_t value) {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return static_cast<uint8_t>(value);
}

void updateIdentityFromWifi() {
  IPAddress ip = WiFi.localIP();
  ipString = ip.toString();
  targetId = buildTargetIdFromIp(ip);
}

bool resolveServerIp() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (WiFi.hostByName(SERVER_HOST, serverIp)) {
    serverIpValid = true;
    return true;
  }
  serverIpValid = false;
  return false;
}

void sendRegister() {
  StaticJsonDocument<896> doc;
  doc["type"] = "target_register";
  doc["target_id"] = targetId;
  doc["display_name"] = targetId;
  doc["udp_port"] = UDP_PORT;
  doc["mode"] = "write_only";

  JsonArray writable = doc.createNestedArray("writable");
  auto addWritable = [&](const char* name) {
    JsonObject v = writable.createNestedObject();
    v["name"] = name;
    v["type"] = "int";
  };
  addWritable("left_speed");
  addWritable("right_speed");
  addWritable("r");
  addWritable("g");
  addWritable("b");

  JsonArray readable = doc.createNestedArray("readable");
  auto addReadable = [&](const char* name, const char* type) {
    JsonObject v = readable.createNestedObject();
    v["name"] = name;
    v["type"] = type;
  };
  addReadable("ip", "string");
  addReadable("status_text", "string");
  addReadable("wifi_rssi", "int");
  addReadable("failsafe", "bool");
  addReadable("last_packet_age_ms", "int");

  JsonObject init = doc.createNestedObject("initial_state");
  init["ip"] = ipString;
  init["status_text"] = statusText;

  String payload;
  serializeJson(doc, payload);
  ws.send(payload);
}

void sendTelemetry() {
  StaticJsonDocument<512> doc;
  doc["type"] = "target_telemetry";
  doc["target_id"] = targetId;

  JsonObject values = doc.createNestedObject("values");
  values["ip"] = ipString;
  values["status_text"] = statusText;
  values["wifi_rssi"] = (int)WiFi.RSSI();
  values["failsafe"] = failsafeActive;
  values["last_packet_age_ms"] = (int)(millis() - lastPacketMs);
  values["left_speed"] = leftSpeed;
  values["right_speed"] = rightSpeed;
  values["r"] = ledR;
  values["g"] = ledG;
  values["b"] = ledB;

  String payload;
  serializeJson(doc, payload);
  ws.send(payload);
}

void handleMessage(const String& message) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, message)) return;
  const char* type = doc["type"] | "";

  if (strcmp(type, "target_registered") == 0) {
    if (doc.containsKey("esp_token")) {
      espToken = doc["esp_token"].as<uint32_t>();
      hasToken = true;
      hasSeq = false;
      setStatus("registered");
    }
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
    switch (event) {
      case WebsocketsEvent::ConnectionOpened:
        wsConnected = true;
        hasToken = false;
        setStatus("ws-connected");
        logLine("WS opened");
        sendRegister();
        break;

      case WebsocketsEvent::ConnectionClosed:
        wsConnected = false;
        hasToken = false;
        setStatus("ws-lost");
        logLine("WS closed");
        zeroOutputs();
        break;

      case WebsocketsEvent::GotPing:
        logLine("WS got ping");
        break;

      case WebsocketsEvent::GotPong:
        logLine("WS got pong");
        break;
    }
  });

  ws.connect(WS_URL);
}

void ensureWebSocket() {
  if (WiFi.status() != WL_CONNECTED) {
    wsConnected = false;
    return;
  }
  if (ws.available()) return;

  unsigned long now = millis();
  if (now - lastWsAttemptMs < WS_RETRY_MS) return;
  lastWsAttemptMs = now;
  connectWebSocket();
}

bool isSeqNewer(uint32_t seq) {
  if (!hasSeq) return true;
  return (int32_t)(seq - lastSeq) > 0;
}

int16_t readInt16LE(const uint8_t* buffer, size_t offset) {
  return (int16_t)(buffer[offset] | (buffer[offset + 1] << 8));
}

void handleUdpPacket(int packetSize) {
  if (!serverIpValid) return;
  if (udp.remoteIP() != serverIp) return;
  if (packetSize < 12) return;

  uint8_t buffer[64];
  int read = udp.read(buffer, sizeof(buffer));
  if (read != packetSize) return;

  uint16_t magic = (uint16_t)(buffer[0] | (buffer[1] << 8));
  uint8_t version = buffer[2];
  uint32_t seq = (uint32_t)buffer[3] | ((uint32_t)buffer[4] << 8) | ((uint32_t)buffer[5] << 16) | ((uint32_t)buffer[6] << 24);
  uint32_t token = (uint32_t)buffer[7] | ((uint32_t)buffer[8] << 8) | ((uint32_t)buffer[9] << 16) | ((uint32_t)buffer[10] << 24);
  uint8_t varCount = buffer[11];
  int expectedSize = 12 + (varCount * 2);

  if (packetSize != expectedSize) return;
  if (magic != UDP_MAGIC || version != UDP_VERSION) return;
  if (!hasToken || token != espToken) return;
  if (varCount != EXPECTED_VAR_COUNT) return;
  if (!isSeqNewer(seq)) return;

  hasSeq = true;
  lastSeq = seq;
  lastPacketMs = millis();
  failsafeActive = false;

  leftSpeed = clampSpeed(readInt16LE(buffer, 12));
  rightSpeed = clampSpeed(readInt16LE(buffer, 14));
  ledR = clampColor(readInt16LE(buffer, 16));
  ledG = clampColor(readInt16LE(buffer, 18));
  ledB = clampColor(readInt16LE(buffer, 20));

  setStatus("running");
  applyOutputs();
}

void pollUdp() {
  int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;
  handleUdpPacket(packetSize);
}

void checkFailsafe() {
  unsigned long now = millis();
  if (!wsConnected || !hasToken) {
    failsafeActive = true;
    zeroOutputs();
    return;
  }
  if (now - lastPacketMs > FAILSAFE_MS) {
    failsafeActive = true;
    setStatus("failsafe");
    zeroOutputs();
  }
}

void updateStatusLed() {
  unsigned long now = millis();
  if (now - lastLedMs < LED_REFRESH_MS) return;
  lastLedMs = now;

  if (failsafeActive || !wsConnected) {
    ledOn = !ledOn;
    applyLedColor(ledOn ? 80 : 0, 0, 0);
    return;
  }

  applyLedColor(0, 0, 40);
}

void printStatus() {
  unsigned long now = millis();
  if (now - lastStatusPrintMs < STATUS_PRINT_MS) return;
  lastStatusPrintMs = now;

  int lastPacketAge = (int)(now - lastPacketMs);

  Serial.print("[");
  Serial.print(now);
  Serial.print("] ");
  Serial.print(statusText);
  Serial.print(" | wifi_rssi=");
  Serial.print(WiFi.status() == WL_CONNECTED ? (int)WiFi.RSSI() : 0);
  Serial.print(" | failsafe=");
  Serial.print(failsafeActive ? "true" : "false");
  Serial.print(" | last_packet_age_ms=");
  Serial.print(lastPacketAge);
  Serial.print(" | left_speed=");
  Serial.print(leftSpeed);
  Serial.print(" | right_speed=");
  Serial.print(rightSpeed);
  Serial.print(" | r=");
  Serial.print(ledR);
  Serial.print(" g=");
  Serial.print(ledG);
  Serial.print(" b=");
  Serial.print(ledB);
  Serial.println();
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

  zeroOutputs();
  setupWifi();
  udp.begin(UDP_PORT);
}

void loop() {
  if (WiFi.status() == WL_CONNECTED && ipString.length() == 0) {
    setStatus("wifi-connected");
    updateIdentityFromWifi();
    resolveServerIp();
  }

  ensureWifi();
  ensureWebSocket();

  ws.poll();
  pollUdp();
  checkFailsafe();
  // updateStatusLed();
  printStatus();

  unsigned long now = millis();
  if (wsConnected && now - lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryMs = now;
    sendTelemetry();
  }
}
