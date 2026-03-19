/**********
  File:   WifiRCCars_v0.1-NoCar
  GitHub: https://github.com/UCDElecSoc/WifiRCCars
  Author: Joe Biju
  Date:   19/03/2026
  Description:
  Simple test script to see if a Xbox controller connected via python to websocket
  can be detected, and use joystick values to drive onboard RGB LED colour.

  Requires:
  - controller_to_esp_v0.1.py
  - WiFi_Credentials.h
  - Adafruit NeoPixel library

  Notes:
  - Assumes a single addressable RGB LED is connected to RGB_LED_PIN
  - Many ESP32-C6 boards use GPIO 8 for the onboard RGB LED, but check your board
**********/

#include <WiFi.h>
#include <WebSocketsServer.h>   // v2.7.2; library is literally called "WebSockets" by Markus Sattler
#include <Adafruit_NeoPixel.h> // v1.15.4
#include "WiFi_Credentials.h"

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;

WebSocketsServer webSocket(81);

// ---------- RGB LED config ----------
#define RGB_LED_PIN    8     // Change if your board uses a different pin
#define RGB_LED_COUNT  1

Adafruit_NeoPixel rgbLed(RGB_LED_COUNT, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

// ---------- Controller state ----------
float leftX  = 0.0;
float leftY  = 0.0;
float rightX = 0.0;
float rightY = 0.0;
int btnA = 0;
int btnB = 0;

// ---------- Helpers ----------
uint8_t axisToByte(float v) {
  // Clamp from [-1, 1] and map to [0, 255]
  if (v < -1.0f) v = -1.0f;
  if (v >  1.0f) v =  1.0f;
  return (uint8_t)((v + 1.0f) * 127.5f);
}

void setRgb(uint8_t r, uint8_t g, uint8_t b) {
  rgbLed.setPixelColor(0, rgbLed.Color(r, g, b));
  rgbLed.show();
}

void updateRgbFromController() {
  // A button overrides everything -> white
  if (btnA) {
    setRgb(255, 255, 255);
    return;
  }

  // Optional: B button turns LED fully off
  if (btnB) {
    setRgb(0, 0, 0);
    return;
  }

  // Map joystick axes to RGB channels
  uint8_t red   = axisToByte(leftX);
  uint8_t green = axisToByte(-leftY);  // invert so pushing up increases green
  uint8_t blue  = axisToByte(rightX);

  setRgb(red, green, blue);
}

void handleMessage(uint8_t num, uint8_t * payload, size_t length) {
  String msg = String((char*)payload);

  // CSV format:
  // lx,ly,rx,ry,a,b
  // Example: 0.12,-0.88,0.01,0.00,1,0

  float vals[6];
  int idx = 0;
  int start = 0;

  for (int i = 0; i <= msg.length(); i++) {
    if (i == msg.length() || msg.charAt(i) == ',') {
      if (idx < 6) {
        vals[idx] = msg.substring(start, i).toFloat();
        idx++;
      }
      start = i + 1;
    }
  }

  if (idx == 6) {
    leftX  = vals[0];
    leftY  = vals[1];
    rightX = vals[2];
    rightY = vals[3];
    btnA   = (int)vals[4];
    btnB   = (int)vals[5];

    Serial.print("LX: "); Serial.print(leftX, 3);
    Serial.print("  LY: "); Serial.print(leftY, 3);
    Serial.print("  RX: "); Serial.print(rightX, 3);
    Serial.print("  RY: "); Serial.print(rightY, 3);
    Serial.print("  A: ");  Serial.print(btnA);
    Serial.print("  B: ");  Serial.println(btnB);

    updateRgbFromController();
  }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected\n", num);
      break;

    case WStype_CONNECTED: {
      IPAddress ip = webSocket.remoteIP(num);
      Serial.printf("[%u] Connected from %d.%d.%d.%d\n", num, ip[0], ip[1], ip[2], ip[3]);
      webSocket.sendTXT(num, "connected");
      break;
    }

    case WStype_TEXT:
      handleMessage(num, payload, length);
      break;

    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);

  // RGB LED init
  rgbLed.begin();
  rgbLed.clear();
  rgbLed.show();

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected");
  Serial.print("ESP32 IP address: ");
  Serial.println(WiFi.localIP());

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  Serial.println("WebSocket server started on port 81");
}

void loop() {
  webSocket.loop();
}