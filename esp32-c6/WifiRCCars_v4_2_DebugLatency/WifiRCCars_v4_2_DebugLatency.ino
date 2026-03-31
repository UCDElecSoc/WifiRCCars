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
WebsocketsClient ws; //creates an instance of a client from <ArduinoWebsockets.h>
//can then attach actions like connect, poll, send, etc. to this instance
Adafruit_NeoPixel pixels(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
//no. of leds, pin number, and type of neopixel (color order RGB and baud rate 800kHz)

String targetId;
String ipString;
String statusText = "booting";

float leftSpeed = 0.0f;   // expected -255..255 (from bindings)
float rightSpeed = 0.0f;  // expected -255..255 (from bindings)

unsigned long lastTelemetryMs = 0;    // Last time telemetry was sent to server.
unsigned long lastWifiAttemptMs = 0;  // Last Wi-Fi reconnect attempt time.
unsigned long lastWsAttemptMs = 0;    // Last WebSocket reconnect attempt time.
unsigned long lastCommandMs = 0;      // Last time a drive command was received.
unsigned long lastStatusPrintMs = 0;  // Last time status was printed to Serial.
unsigned long lastLedToggleMs = 0;    // Last status LED toggle timestamp.
bool wsConnected = false;             // True when WebSocket session is currently open.
bool ledOn = false;                   // Current blink phase for status LED.

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
  uint32_t color = pixels.Color(r, g, b); //builds a 32-bit color value
  pixels.setPixelColor(0, color); //sets the color of pixel 0 (the only pixel) in the strip
  pixels.show(); //push buffer to data pin
}

uint8_t clampPwm(float value) {
  int v = (int)roundf(fabs(value)); //floating point abs()
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


/*
  Updates the target's identity based on its current Wi-Fi IP address. 
  The IP address is converted to a string and used to build a unique target ID. 
  This allows the server to identify this specific device when it connects.
*/
void updateIdentityFromWifi() {
  IPAddress ip = WiFi.localIP();
  ipString = ip.toString(); //note: .toString() is a method of class IPAddress
  targetId = buildTargetIdFromIp(ip);
}

/*
  Sends a registration message to the server with the target's identity, 
  variable definitions, and initial state. 
  This is called by connectWebSocket when the connection is established to let the server 
  know about this target and what variables it has for control and telemetry.
*/
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
  serializeJson(doc, payload); //converts json document to string and stores in payload variable
  ws.send(payload); //sends the payload string over the WebSocket
}

/*
  Sends telemetry data to the server, including the target's IP address, 
  status text, and Wi-Fi signal strength (RSSI).
  Only function to call ws.send() besides sendRegister
*/
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

/*
  Handles incoming messages from the server. 
  It parses the message as JSON and checks the "type" field to determine the action to take.
*/
void handleMessage(const String& message) {
  StaticJsonDocument<768> doc;
  DeserializationError err = deserializeJson(doc, message); //msg from server parsed into "doc" json document
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


/*
  Sets up the Wi-Fi connection to the network.
*/
void setupWifi() {
  setStatus("wifi-connecting");
  WiFi.mode(WIFI_STA); //station mode (client)
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

/*
  Ensures that the Wi-Fi connection is active. 
  - If already connected, it does nothing.
  - If not connected, it checks if enough time has passed since the last connection attempt 
  and tries to reconnect if needed.
*/
void ensureWifi() {
  //Wifi status can return: WL_CONNECTED, WL_NO_SSID_AVAIL, WL_CONNECT_FAILED, WL_IDLE_STATUS, WL_DISCONNECTED, WIFI_CONNECTION_LOST
  if (WiFi.status() == WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastWifiAttemptMs < WIFI_RETRY_MS) return;
  lastWifiAttemptMs = now;
  setStatus("wifi-connecting");
  WiFi.disconnect(true); //disconnect from current bad state
  WiFi.begin(WIFI_SSID, WIFI_PASS); //try to connect again
}


/*
  Sets up the WebSocket connection to the server and defines callbacks 
  for handling messages and connection events.
*/
void connectWebSocket() {
  setStatus("ws-connecting");


  /*
    Create callbacks for WebSocket events before connecting.
  */

  /*
    WebSocket message handler: when a message is received, 
    the provided lambda function is called with the message as an argument.
  */
  ws.onMessage([](WebsocketsMessage msg) {
    handleMessage(msg.data());
  });

  /*
    WebSocket event handler: when a connection event occurs (like opened or closed),
    the provided lambda function is called with the event type and optional data.
  */
  ws.onEvent([](WebsocketsEvent event, String data) { //data is unused here but could contain info about the event
    /*
      When the WebSocket connection is opened (successful handshake),
      run following code
    */
    if (event == WebsocketsEvent::ConnectionOpened) {
      wsConnected = true;
      setStatus("ws-connected");
      sendRegister(); //send registration message to server to announce presence and variable definitions
    } 
    
    /*
      When the WebSocket connection is closed (disconnected), 
      run following code
    */
    else if (event == WebsocketsEvent::ConnectionClosed) {
      wsConnected = false;
      setStatus("ws-lost");
      applySafeOutputs();
    }
  });


  /*
    Finally, initiate the WebSocket connection to the server URL. 
    The callbacks set above will handle the connection events and incoming messages.
  */
  ws.connect(WS_URL);
}

/*
  Ensures that the WebSocket connection is active. 
  - If Wi-Fi is not connected, it sets the status to "wifi-lost" and marks the WebSocket as disconnected.
  - If Wi-Fi is connected but the WebSocket is not available, it checks if enough time has passed since 
  the last connection attempt and tries to reconnect if needed.
  - On connection, connectWebSocket() will set callbacks
*/
void ensureWebSocket() {
  if (WiFi.status() != WL_CONNECTED) { //no wifi, definitely no websocket
    setStatus("wifi-lost");
    wsConnected = false;
    return;
  }
  if (ws.available()) return; //websocket is working, no need to reconnect

  unsigned long now = millis();
  if (now - lastWsAttemptMs < WS_RETRY_MS) return;
  lastWsAttemptMs = now;
  connectWebSocket(); //retry to set up callbacks and connect websocket
}

/*
 Checks the failsafe condition and applies safe outputs if necessary.
*/
void checkFailsafe() {
  unsigned long now = millis();
  if (wsConnected && (now - lastCommandMs > COMMAND_TIMEOUT_MS)) {
    setStatus("failsafe");
    applySafeOutputs();
  }
}

/*
  Updates the status GPIO8 NEOPIXEL RGB LED based on the connection status.
  - If fully connected (Wi-Fi and WebSocket), the LED flashes blue slowly.
  - If disconnected (either Wi-Fi or WebSocket), the LED flashes red quickly.
*/
void updateStatusLed() {
  unsigned long now = millis();
  bool connected = (WiFi.status() == WL_CONNECTED) && wsConnected;
  unsigned long interval = connected ? LED_FLASH_CONNECTED_MS : LED_FLASH_DISCONNECTED_MS;
  if (now - lastLedToggleMs < interval) return;
  lastLedToggleMs = now;
  ledOn = !ledOn;

  // Slow flash blue when full connected (wifi and ws)
  if (connected) {
    if (ledOn) {
      applyLedColor(0, 0, 80); //blue
    } else {
      applyLedColor(0, 0, 0);
    }
  } 
  
  // Fast flash red when disconnected (either wifi or ws)
  else {
    if (ledOn) {
      applyLedColor(80, 0, 0); //red
    } else {
      applyLedColor(0, 0, 0);
    }
  }
}

/*
  Prints the current status of the system to the Serial console, including:
  - Timestamp in milliseconds
  - Custom status text set by setStatus()
  - Wi-Fi connection status and RSSI
  - WebSocket connection status
  - Current motor speeds
  - LED color status based on connection
  - IP address of the device
*/
void printStatus() {
  unsigned long now = millis();
  if (now - lastStatusPrintMs < STATUS_PRINT_MS) return;
  lastStatusPrintMs = now;

  Serial.print("[");
  Serial.print(now); //time in ms
  Serial.print("] [status] ");
  Serial.print(statusText); //from setStatus() calls throughout the code
  Serial.print(" | wifi=");
  Serial.print(WiFi.status() == WL_CONNECTED ? "ok" : "lost"); //wifi connection status
  Serial.print(" rssi=");
  Serial.print(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0); //wifi RSSI
  Serial.print(" | ws=");
  Serial.print(wsConnected ? "ok" : "lost"); //websocket connection status
  Serial.print(" | left=");
  Serial.print(leftSpeed); //left motor speed
  Serial.print(" right=");
  Serial.print(rightSpeed); //right motor speed
  Serial.print(" | led=");
  Serial.print((WiFi.status() == WL_CONNECTED) && wsConnected ? "blue" : "red"); //led color based on connection status
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
