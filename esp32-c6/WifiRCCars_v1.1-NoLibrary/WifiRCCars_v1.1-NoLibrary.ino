#include <WiFi.h>
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include <Adafruit_NeoPixel.h>
#include "../WiFi_Credentials.h" //file should contain WiFi Passwords; separated to gitignore

using namespace websockets;

// ====== User Config ======
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;

// WebSocket URL for your backend server.
const char* WS_URL = "ws://192.168.50.50:8000/ws";

// NeoPixel config
constexpr int NEOPIXEL_PIN = 8;
constexpr int NEOPIXEL_COUNT = 1;

// Telemetry interval (ms)
constexpr unsigned long TELEMETRY_INTERVAL_MS = 3000;

// ====== State ======
WebsocketsClient ws;
Adafruit_NeoPixel pixels(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

String targetId;
String ipString;
String statusText = "booting";

int rVal = 0;
int gVal = 0;
int bVal = 0;

unsigned long lastTelemetryMs = 0;

// ====== Helpers ======
String buildTargetIdFromIp(IPAddress ip) {
  int last = ip[3];
  int idx = last - 100;
  if (idx < 0) idx = 0;
  if (idx > 99) idx = 99;
  char buf[8];
  // zero-pad 2 digits
  snprintf(buf, sizeof(buf), "ESP-%02d", idx);
  return String(buf);
}

void applyLed() {
  uint32_t color = pixels.Color(
    constrain(rVal, 0, 255),
    constrain(gVal, 0, 255),
    constrain(bVal, 0, 255)
  );
  pixels.setPixelColor(0, color);
  pixels.show();
}

void sendRegister() {
  StaticJsonDocument<512> doc;
  doc["type"] = "target_register";
  doc["target_id"] = targetId;

  JsonArray vars = doc.createNestedArray("variables");

  auto addVar = [&](const char* name, const char* type, const char* access) {
    JsonObject v = vars.createNestedObject();
    v["name"] = name;
    v["type"] = type;
    v["access"] = access;
  };

  addVar("r", "int", "write");
  addVar("g", "int", "write");
  addVar("b", "int", "write");
  addVar("ip", "string", "read");
  addVar("statusText", "string", "read");

  JsonObject init = doc.createNestedObject("initial_state");
  init["ip"] = ipString;
  init["statusText"] = statusText;

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
  values["statusText"] = statusText;

  String payload;
  serializeJson(doc, payload);
  ws.send(payload);
}

void handleMessage(const String& message) {
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, message);
  if (err) {
    return;
  }

  const char* type = doc["type"] | "";
  if (strcmp(type, "target_write_update") == 0) {
    const char* msgTarget = doc["target_id"] | "";
    if (targetId != msgTarget) return;

    JsonObject values = doc["values"];
    if (!values.isNull()) {
      if (values.containsKey("r")) rVal = values["r"];
      if (values.containsKey("g")) gVal = values["g"];
      if (values.containsKey("b")) bVal = values["b"];
      statusText = "ok";
      applyLed();
    }
  } else if (strcmp(type, "target_registered") == 0) {
    statusText = "registered";
  }
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  IPAddress ip = WiFi.localIP();
  ipString = ip.toString();
  targetId = buildTargetIdFromIp(ip);
}

void connectWebSocket() {
  ws.onMessage([](WebsocketsMessage msg) {
    handleMessage(msg.data());
  });

  ws.onEvent([](WebsocketsEvent event, String data) {
    if (event == WebsocketsEvent::ConnectionOpened) {
      statusText = "ws-connected";
      sendRegister();
    } else if (event == WebsocketsEvent::ConnectionClosed) {
      statusText = "ws-closed";
    }
  });

  ws.connect(WS_URL);
}

void setup() {
  pixels.begin();
  pixels.clear();
  pixels.show();

  connectWifi();
  connectWebSocket();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    statusText = "wifi-reconnect";
    connectWifi();
    connectWebSocket();
  }

  if (!ws.available()) {
    // Attempt to reconnect if socket dropped
    ws.connect(WS_URL);
    delay(100);
  }

  ws.poll();

  unsigned long now = millis();
  if (now - lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryMs = now;
    sendTelemetry();
  }
}
