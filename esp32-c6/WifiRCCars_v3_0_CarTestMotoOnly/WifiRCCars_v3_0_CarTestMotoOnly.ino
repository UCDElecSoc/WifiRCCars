/****
Expected outputs. Physically rewire if not matching.

//1000 spins left motor reverse
//0100 spins left motor forward
//0010 spins right motor reverse
//0001 spins right motor forward
//1100 or 0011 turns off both motors
//0000 turns off both motors
//0101 spins both forward
//1010 spins both backward
****/

// #include <WiFi.h>
// #include <ArduinoJson.h>
// #include <ArduinoWebsockets.h>
#include <Adafruit_NeoPixel.h>
// #include "../WiFi_Credentials.h"

const int IN1 = 11;
const int IN2 = 10;
const int IN3 = 20;
const int IN4 = 21;

// ESP32-C6 onboard RGB LED is commonly on GPIO8
const int RGB_PIN = 8;
const int RGB_COUNT = 1;

Adafruit_NeoPixel pixel(RGB_COUNT, RGB_PIN, NEO_GRB + NEO_KHZ800);

// Default output state
String currentState = "0000";

// LED blink timing
unsigned long lastBlinkMs = 0;
const unsigned long blinkIntervalMs = 500;
bool ledOn = false;

void applyState(const String& s) {
  digitalWrite(IN1, s[0] == '1' ? HIGH : LOW);
  digitalWrite(IN2, s[1] == '1' ? HIGH : LOW);
  digitalWrite(IN3, s[2] == '1' ? HIGH : LOW);
  digitalWrite(IN4, s[3] == '1' ? HIGH : LOW);

  Serial.println("Received: " + s);
  Serial.println("Applying:");
  Serial.println("  IN1 = " + String(s[0] == '1' ? "HIGH" : "LOW"));
  Serial.println("  IN2 = " + String(s[1] == '1' ? "HIGH" : "LOW"));
  Serial.println("  IN3 = " + String(s[2] == '1' ? "HIGH" : "LOW"));
  Serial.println("  IN4 = " + String(s[3] == '1' ? "HIGH" : "LOW"));
  Serial.println();
}

bool isValidInput(const String& s) {
  if (s.length() != 4) return false;

  for (int i = 0; i < 4; i++) {
    if (s[i] != '0' && s[i] != '1') return false;
  }
  return true;
}

void handleSerial() {
  if (!Serial.available()) return;

  String input = Serial.readStringUntil('\n');
  input.trim();

  if (isValidInput(input)) {
    currentState = input;
    applyState(currentState);
  } else {
    Serial.println("Invalid input: " + input);
    Serial.println("Enter exactly 4 bits, e.g. 1000 or 0110");
    Serial.println();
  }
}

void updateBlueHeartbeat() {
  unsigned long now = millis();

  if (now - lastBlinkMs >= blinkIntervalMs) {
    lastBlinkMs = now;
    ledOn = !ledOn;

    if (ledOn) {
      pixel.setPixelColor(0, pixel.Color(0, 0, 40));  // blue
    } else {
      pixel.setPixelColor(0, pixel.Color(0, 0, 0));   // off
    }
    pixel.show();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  pixel.begin();
  pixel.clear();
  pixel.show();

  // Default to 0000 on boot
  applyState(currentState);

  Serial.println("Ready.");
  Serial.println("Type 4 bits into Serial Monitor, e.g.:");
  Serial.println("1000");
  Serial.println("0110");
  Serial.println();
}

void loop() {
  handleSerial();
  updateBlueHeartbeat();
}