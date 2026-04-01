#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include "../WiFi_Credentials.h"

using namespace websockets;

// ====== User Config ======
const char* SERVER_HOST = "192.168.50.51";
const char* WS_URL = "ws://192.168.50.51:8000/ws";
constexpr uint16_t UDP_PORT = 4210;

// UDP protocol constants
constexpr uint16_t UDP_MAGIC = 0xCAFE;
constexpr uint8_t UDP_VERSION = 1;
constexpr uint8_t EXPECTED_VAR_COUNT = 2;

// ====== State ======
WebsocketsClient ws;
WiFiUDP udp;

bool wsOpen = false;
bool wsEverConnected = false;
bool wsFailed = false;

bool hasToken = false;
uint32_t espToken = 0;

bool hasSeq = false;
uint32_t lastSeq = 0;

int16_t leftSpeed = 0;
int16_t rightSpeed = 0;

IPAddress serverIp;
bool serverIpValid = false;

String targetId;
String displayName;
String ipString;

unsigned long lastHeartbeatMs = 0;
unsigned long lastPacketMs = 0;
unsigned long lastRejectLogMs = 0;
constexpr unsigned long REJECT_LOG_MS = 500;

// ====== Helpers ======
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

String buildTargetIdFromIp(const IPAddress& ip) {
  int last = ip[3];
  int suffix = last % 100;
  if (suffix < 0) suffix = 0;
  if (suffix > 99) suffix = 99;
  char buf[8];
  snprintf(buf, sizeof(buf), "esp-%02d", suffix);
  return String(buf);
}

bool connectWifi() {
  logLine("wifi connect attempt");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 20000) {
    delay(200);
  }

  if (WiFi.status() != WL_CONNECTED) {
    logLine("wifi failed");
    return false;
  }

  IPAddress ip = WiFi.localIP();
  ipString = ip.toString();
  targetId = buildTargetIdFromIp(ip);
  displayName = targetId;

  Serial.print("[");
  Serial.print(millis());
  Serial.print("] wifi connected ip=");
  Serial.println(ipString);
  return true;
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

bool isSeqNewer(uint32_t seq) {
  if (!hasSeq) return true;
  return (int32_t)(seq - lastSeq) > 0;
}

int16_t readInt16LE(const uint8_t* buffer, size_t offset) {
  return (int16_t)(buffer[offset] | (buffer[offset + 1] << 8));
}

void handleMessage(const String& message) {
  Serial.print("[");
  Serial.print(millis());
  Serial.print("] ws msg ");
  Serial.println(message);

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, message)) return;
  const char* type = doc["type"] | "";
  if (strcmp(type, "target_registered") == 0) {
    if (doc.containsKey("esp_token")) {
      espToken = doc["esp_token"].as<uint32_t>();
      hasToken = true;
      hasSeq = false;
      Serial.print("[");
      Serial.print(millis());
      Serial.print("] esp_token=");
      Serial.println(espToken);
    }
  }
}

void sendRegister() {
  StaticJsonDocument<256> doc;
  doc["type"] = "target_register";
  doc["target_id"] = targetId;
  doc["display_name"] = displayName;
  doc["udp_port"] = UDP_PORT;

  JsonArray writable = doc.createNestedArray("writable");
  JsonObject v1 = writable.createNestedObject();
  v1["name"] = "left_speed";
  v1["type"] = "int";
  JsonObject v2 = writable.createNestedObject();
  v2["name"] = "right_speed";
  v2["type"] = "int";

  String payload;
  serializeJson(doc, payload);

  Serial.print("[");
  Serial.print(millis());
  Serial.print("] register json=");
  Serial.println(payload);

  ws.send(payload);
}

void connectWebSocket() {
  logLine("ws connect attempt");

  ws.onMessage([](WebsocketsMessage msg) {
    handleMessage(msg.data());
  });

  ws.onEvent([](WebsocketsEvent event, String data) {
    switch (event) {
      case WebsocketsEvent::ConnectionOpened:
        wsOpen = true;
        wsEverConnected = true;
        logLine("ws opened");
        sendRegister();
        break;
      case WebsocketsEvent::ConnectionClosed:
        wsOpen = false;
        logLine("ws closed");
        break;
      case WebsocketsEvent::GotPing:
        logLine("ws got ping");
        break;
      case WebsocketsEvent::GotPong:
        logLine("ws got pong");
        break;
    }
  });

  bool ok = ws.connect(WS_URL);
  logLine(ok ? "ws connect ok" : "ws connect failed");
  if (!ok) {
    wsFailed = true;
  }
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

  uint8_t buffer[64];
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

  leftSpeed = clamp(readInt16LE(buffer, 12), -32768, 32767);
  rightSpeed = clamp(readInt16LE(buffer, 14), -32768, 32767);

  Serial.print("[");
  Serial.print(millis());
  Serial.print("] udp_ok seq=");
  Serial.print(seq);
  Serial.print(" left=");
  Serial.print(leftSpeed);
  Serial.print(" right=");
  Serial.println(rightSpeed);
}

void pollUdp() {
  if (!hasToken) return;
  int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;
  handleUdpPacket(packetSize);
}

void printHeartbeat() {
  unsigned long now = millis();
  if (now - lastHeartbeatMs < 1000) return;
  lastHeartbeatMs = now;

  unsigned long age = (lastPacketMs == 0) ? 0 : (now - lastPacketMs);

  Serial.print("[");
  Serial.print(now);
  Serial.print("] hb wifi=");
  Serial.print(WiFi.status() == WL_CONNECTED ? "ok" : "lost");
  Serial.print(" ws=");
  Serial.print(wsOpen ? "open" : "closed");
  Serial.print(" token=");
  Serial.print(hasToken ? "true" : "false");
  Serial.print(" seq=");
  Serial.print(hasSeq ? String(lastSeq) : String("-"));
  Serial.print(" last_age_ms=");
  Serial.print(age);
  Serial.print(" left=");
  Serial.print(leftSpeed);
  Serial.print(" right=");
  Serial.println(rightSpeed);
}

void setup() {
  Serial.begin(115200);
  Serial.print("[");
  Serial.print(millis());
  Serial.print("] EXPECTED_VAR_COUNT=");
  Serial.println(EXPECTED_VAR_COUNT);
  logLine("boot");

  if (connectWifi()) {
    resolveServerIp();
    udp.begin(UDP_PORT);
    connectWebSocket();
  }
}

void loop() {
  if (wsOpen || wsEverConnected) {
    ws.poll();
  }

  pollUdp();
  printHeartbeat();

  if (wsFailed || (!wsOpen && wsEverConnected)) {
    delay(5);
  }
}
