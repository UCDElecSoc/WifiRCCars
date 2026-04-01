#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include <Adafruit_NeoPixel.h>
#include "../WiFi_Credentials.h" //private file with WIFI_SSID and WIFI_PASS defined

using namespace websockets;

// ====== User Config ======
const char* SERVER_HOST = "192.168.50.50";
const char* WS_URL = "ws://192.168.50.50:8000/ws";

constexpr uint16_t UDP_PORT = 4210;
constexpr uint16_t UDP_MAGIC = 0xCAFE;
constexpr uint8_t UDP_VERSION = 1;
constexpr uint8_t EXPECTED_VAR_COUNT = 2;

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

// analogWrite() on Arduino-ESP32 already provides PWM output, so no manual ledcAttach() is needed here.
constexpr uint8_t PWM_MAX = 255;

constexpr unsigned long WIFI_RETRY_MS = 2000;
constexpr unsigned long WS_RETRY_MS = 2000;
constexpr unsigned long FAILSAFE_MS = 300;
constexpr unsigned long LED_REFRESH_MS = 120;
constexpr unsigned long STATUS_PRINT_MS = 250;
constexpr unsigned long REJECT_LOG_MS = 500;

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

uint32_t espToken = 0;
bool hasToken = false;

unsigned long lastWifiAttemptMs = 0;
unsigned long lastWsAttemptMs = 0;
unsigned long lastPacketMs = 0;
unsigned long lastLedMs = 0;
unsigned long lastStatusPrintMs = 0;
unsigned long lastRejectLogMs = 0;
bool wsConnected = false;
bool wsConnectInProgress = false;
bool failsafeActive = true;
bool ledOn = false;
bool hasSeq = false;
uint32_t lastSeq = 0;
bool identityReady = false;

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

void logReject(const char* reason) {
  unsigned long now = millis();
  if (now - lastRejectLogMs < REJECT_LOG_MS) return;
  lastRejectLogMs = now;
  Serial.print("[");
  Serial.print(now);
  Serial.print("] udp_drop ");
  Serial.println(reason);
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
  int16_t clamped = speed;
  if (clamped > PWM_MAX) clamped = PWM_MAX;
  if (clamped < -PWM_MAX) clamped = -PWM_MAX;

  uint8_t duty = (uint8_t)abs(clamped);

  if (clamped > 0) {
    analogWrite(pinForward, duty);
    analogWrite(pinReverse, 0);
  } else if (clamped < 0) {
    analogWrite(pinForward, 0);
    analogWrite(pinReverse, duty);
  } else {
    analogWrite(pinForward, 0);
    analogWrite(pinReverse, 0);
  }
}

void applyOutputs() {
  applyMotorSide(leftSpeed, IN2, IN1);
  applyMotorSide(rightSpeed, IN4, IN3);
}

void zeroOutputs() {
  leftSpeed = 0;
  rightSpeed = 0;
  applyOutputs();
}

int16_t clampSpeed(int16_t value) {
  if (value > PWM_MAX) return PWM_MAX;
  if (value < -PWM_MAX) return -PWM_MAX;
  return value;
}

void initIdentityIfReady() {
  if (identityReady) return;
  if (WiFi.status() != WL_CONNECTED) return;
  IPAddress ip = WiFi.localIP();
  ipString = ip.toString();
  targetId = buildTargetIdFromIp(ip);
  identityReady = true;
  Serial.print("[");
  Serial.print(millis());
  Serial.print("] identity ready. Target ID: ");
  Serial.println(targetId);
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
  StaticJsonDocument<512> doc;
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

  // Telemetry is intentionally disabled for debugging WebSocket stability.
  JsonObject init = doc.createNestedObject("initial_state");
  init["ip"] = ipString;
  init["status_text"] = statusText;

  String payload;
  serializeJson(doc, payload);
  ws.send(payload);
}

void handleMessage(const String& message) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, message)) return;
  const char* type = doc["type"] | "";

  if (strcmp(type, "target_registered") == 0) {
    logLine("target_registered");
    if (doc.containsKey("esp_token")) {
      espToken = doc["esp_token"].as<uint32_t>();
      hasToken = true;
      hasSeq = false;
      setStatus("registered");
      Serial.print("[");
      Serial.print(millis());
      Serial.print("] esp_token=");
      Serial.println(espToken);
    }
  }
}

void setupWifi() {
  setStatus("wifi-connecting");
  logLine("wifi connect");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastWifiAttemptMs < WIFI_RETRY_MS) return;
  lastWifiAttemptMs = now;
  setStatus("wifi-connecting");
  logLine("wifi reconnect");
  WiFi.disconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void setupWebSocketHandlers() {
  // Register handlers once to avoid duplicate callbacks on reconnect.
  ws.onMessage([](WebsocketsMessage msg) {
    handleMessage(msg.data());
  });

  ws.onEvent([](WebsocketsEvent event, String data) {
    switch (event) {
      case WebsocketsEvent::ConnectionOpened:
        wsConnected = true;
        wsConnectInProgress = false;
        hasToken = false;
        setStatus("ws-connected");
        logLine("ws opened");
        sendRegister();
        break;

      case WebsocketsEvent::ConnectionClosed:
        wsConnected = false;
        wsConnectInProgress = false;
        hasToken = false;
        setStatus("ws-lost");
        logLine("ws closed");
        zeroOutputs();
        break;

      case WebsocketsEvent::GotPing:
        logLine("ws got ping");
        break;

      case WebsocketsEvent::GotPong:
        logLine("ws got pong");
        break;
    }
  });
}

void resetWebSocket() {
  ws = WebsocketsClient();
  setupWebSocketHandlers();
}

void connectWebSocket() {
  if (wsConnected || wsConnectInProgress) {
    logLine("ws connect suppressed");
    return;
  }
  wsConnectInProgress = true;
  setStatus("ws-connecting");
  logLine("ws connect attempt");
  resetWebSocket();
  bool ok = ws.connect(WS_URL);
  logLine(ok ? "ws connect ok" : "ws connect failed");
  if (!ok) {
    wsConnectInProgress = false;
  }
}

void ensureWebSocket() {
  if (WiFi.status() != WL_CONNECTED) {
    wsConnected = false;
    wsConnectInProgress = false;
    return;
  }
  if (wsConnected || wsConnectInProgress) return;

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
  if (!serverIpValid) {
    logReject("no_server_ip");
    return;
  }
  if (udp.remoteIP() != serverIp) {
    logReject("src_ip");
    return;
  }
  if (packetSize < 12) {
    logReject("size_small");
    return;
  }

  uint8_t buffer[32];
  int read = udp.read(buffer, sizeof(buffer));
  if (read != packetSize) {
    logReject("size_mismatch");
    return;
  }

  uint16_t magic = (uint16_t)(buffer[0] | (buffer[1] << 8));
  uint8_t version = buffer[2];
  uint32_t seq = (uint32_t)buffer[3] | ((uint32_t)buffer[4] << 8) | ((uint32_t)buffer[5] << 16) | ((uint32_t)buffer[6] << 24);
  uint32_t token = (uint32_t)buffer[7] | ((uint32_t)buffer[8] << 8) | ((uint32_t)buffer[9] << 16) | ((uint32_t)buffer[10] << 24);
  uint8_t varCount = buffer[11];
  int expectedSize = 12 + (varCount * 2);

  if (packetSize != expectedSize) {
    logReject("size_exact");
    return;
  }
  if (magic != UDP_MAGIC) {
    logReject("magic");
    return;
  }
  if (version != UDP_VERSION) {
    logReject("version");
    return;
  }
  if (!hasToken || token != espToken) {
    logReject("token");
    return;
  }
  if (varCount != EXPECTED_VAR_COUNT) {
    logReject("var_count");
    return;
  }
  if (!isSeqNewer(seq)) {
    logReject("seq");
    return;
  }

  hasSeq = true;
  lastSeq = seq;
  lastPacketMs = millis();
  failsafeActive = false;

  leftSpeed = clampSpeed(readInt16LE(buffer, 12));
  rightSpeed = clampSpeed(readInt16LE(buffer, 14));

  Serial.print("[");
  Serial.print(millis());
  Serial.print("] udp_ok seq=");
  Serial.print(seq);
  Serial.print(" left=");
  Serial.print(leftSpeed);
  Serial.print(" right=");
  Serial.println(rightSpeed);

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
    if (!failsafeActive) {
      logLine("failsafe (ws/token)");
    }
    failsafeActive = true;
    zeroOutputs();
    return;
  }
  if (now - lastPacketMs > FAILSAFE_MS) {
    if (!failsafeActive) {
      logLine("failsafe (timeout)");
    }
    failsafeActive = true;
    setStatus("failsafe");
    zeroOutputs();
  }
}

void setStatusLedState() {
  // LED meanings:
  // - Blink red: Wi-Fi disconnected
  // - Blink blue: Wi-Fi connected, failsafe active
  // - Solid green: valid control packets, failsafe inactive
  unsigned long now = millis();
  if (now - lastLedMs < LED_REFRESH_MS) return;
  lastLedMs = now;

  bool wifiOk = WiFi.status() == WL_CONNECTED;
  if (!wifiOk) {
    ledOn = !ledOn;
    applyLedColor(ledOn ? 80 : 0, 0, 0);
    return;
  }

  if (failsafeActive) {
    ledOn = !ledOn;
    applyLedColor(0, 0, ledOn ? 80 : 0);
    return;
  }

  applyLedColor(0, 80, 0);
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
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  Serial.print("[");
  Serial.print(millis());
  Serial.print("] EXPECTED_VAR_COUNT=");
  Serial.println(EXPECTED_VAR_COUNT);

  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  pixels.begin();
  pixels.clear();
  pixels.show();

  setupWebSocketHandlers();
  zeroOutputs();
  setupWifi();
  udp.begin(UDP_PORT);
}

void loop() {
  initIdentityIfReady();
  resolveServerIp();

  ensureWifi();
  ensureWebSocket();

  ws.poll();
  pollUdp();
  checkFailsafe();
  setStatusLedState();
  printStatus();
}

