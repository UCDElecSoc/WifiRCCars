/*
  ESP32-C6 onboarding / test sketch
  ---------------------------------
  What it does:
  - blinks/tests the addressable LED on GPIO8
  - connects to Wi-Fi
  - prints chip info, MAC, IP, hostname-style name
  - derives espN from last IP byte (e.g. .101 -> esp1)
  - keeps LED indicating status

  LED status:
  - boot/test: cycles red -> green -> blue
  - connecting Wi-Fi: yellow blink
  - connected + valid espN: solid green
  - connected but IP outside expected range: solid blue
  - Wi-Fi failed / disconnected: solid red

  Board target:
  - ESP32-C6 with single WS2812-style addressable LED on GPIO8
*/

#include <WiFi.h>
#include <Adafruit_NeoPixel.h>
#include "../WiFi_Credentials.h" //file should contain WiFi Passwords; separated to gitignore

// =========================
// USER CONFIG
// =========================
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;

// Expected subnet pattern for naming:
// 192.168.x.101 -> esp1
// 192.168.x.102 -> esp2
// ...
static const int IP_BASE = 100;  // last octet minus this gives esp number

// Wi-Fi connect timeout
static const unsigned long WIFI_TIMEOUT_MS = 15000;

// =========================
// LED CONFIG
// =========================
static const int LED_PIN = 8;
static const int LED_COUNT = 1;
Adafruit_NeoPixel pixel(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// =========================
// HELPERS
// =========================
void setLed(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

void blinkLed(uint8_t r, uint8_t g, uint8_t b, int onMs, int offMs, int times) {
  for (int i = 0; i < times; i++) {
    setLed(r, g, b);
    delay(onMs);
    setLed(0, 0, 0);
    delay(offMs);
  }
}

void ledBootTest() {
  setLed(0, 0, 0);
  delay(100);
  setLed(50, 0, 0);   // red
  delay(250);
  setLed(0, 50, 0);   // green
  delay(250);
  setLed(0, 0, 50);   // blue
  delay(250);
  setLed(0, 0, 0);
  delay(100);
}

String getMacString() {
  return WiFi.macAddress();
}

String ipToString(IPAddress ip) {
  return ip.toString();
}

String deriveNameFromIP(IPAddress ip, bool& validName, int& unitNumber) {
  int last = ip[3];
  unitNumber = last - IP_BASE;
  validName = (unitNumber >= 1 && unitNumber <= 999);
  if (validName) {
    return "esp" + String(unitNumber);
  }
  return "esp-unknown";
}

void printBanner() {
  Serial.println();
  Serial.println("======================================");
  Serial.println("ESP32-C6 ONBOARDING / NETWORK TEST");
  Serial.println("======================================");
}

void printChipInfo() {
  Serial.printf("Chip model: %s\n", ESP.getChipModel());
  Serial.printf("Chip revision: %u\n", ESP.getChipRevision());
  Serial.printf("CPU freq MHz: %u\n", ESP.getCpuFreqMHz());
  Serial.printf("Flash size bytes: %u\n", ESP.getFlashChipSize());
}

bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  Serial.printf("Connecting to SSID: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  bool ledState = false;

  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    ledState = !ledState;
    if (ledState) {
      setLed(40, 25, 0);  // yellow-ish
    } else {
      setLed(0, 0, 0);
    }
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  return WiFi.status() == WL_CONNECTED;
}

void printNetworkInfo() {
  bool validName = false;
  int unitNumber = -1;
  IPAddress ip = WiFi.localIP();
  String derivedName = deriveNameFromIP(ip, validName, unitNumber);

  Serial.println("----- NETWORK INFO -----");
  Serial.printf("MAC: %s\n", getMacString().c_str());
  Serial.printf("Hostname-derived name: %s\n", derivedName.c_str());
  Serial.printf("Assigned IP: %s\n", ipToString(ip).c_str());
  Serial.printf("Gateway: %s\n", ipToString(WiFi.gatewayIP()).c_str());
  Serial.printf("Subnet mask: %s\n", ipToString(WiFi.subnetMask()).c_str());
  Serial.printf("RSSI dBm: %d\n", WiFi.RSSI());
  Serial.printf("Wi-Fi channel: %d\n", WiFi.channel());

  if (validName) {
    Serial.printf("Derived unit number: %d\n", unitNumber);
    Serial.println("Name is valid for your reserved-IP scheme.");
  } else {
    Serial.println("WARNING: IP does not match expected .101+ naming scheme.");
  }

  Serial.println("------------------------");
}

void setFinalLedState() {
  if (WiFi.status() != WL_CONNECTED) {
    setLed(50, 0, 0);  // red
    return;
  }

  bool validName = false;
  int unitNumber = -1;
  deriveNameFromIP(WiFi.localIP(), validName, unitNumber);

  if (validName) {
    setLed(0, 50, 0);  // green
  } else {
    setLed(0, 0, 50);  // blue
  }
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  pixel.begin();
  pixel.setBrightness(32);

  printBanner();
  printChipInfo();

  Serial.println("Running LED test on GPIO8...");
  ledBootTest();
  blinkLed(40, 40, 40, 120, 120, 2);

  Serial.printf("Factory STA MAC: %s\n", getMacString().c_str());

  bool connected = connectWifi();

  if (connected) {
    printNetworkInfo();
  } else {
    Serial.println("Wi-Fi connection FAILED or timed out.");
    Serial.printf("MAC: %s\n", getMacString().c_str());
  }

  setFinalLedState();

  Serial.println();
  Serial.println("READY.");
  Serial.println("Copy MAC/IP/name from Serial Monitor into spreadsheet.");
}

void loop() {
  static unsigned long lastCheck = 0;
  static unsigned long lastHeartbeat = 0;
  static bool blinkState = false;

  unsigned long now = millis();

  // Re-check Wi-Fi every 2 seconds
  if (now - lastCheck >= 2000) {
    lastCheck = now;

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Wi-Fi lost. Attempting reconnect...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }

  // LED behaviour
  if (WiFi.status() == WL_CONNECTED) {
    bool validName = false;
    int unitNumber = -1;
    deriveNameFromIP(WiFi.localIP(), validName, unitNumber);

    if (validName) {
      setLed(0, 50, 0);  // solid green
    } else {
      setLed(0, 0, 50);  // solid blue
    }

    if (now - lastHeartbeat >= 5000) {
      lastHeartbeat = now;
      Serial.printf("[OK] IP=%s RSSI=%d NameCheck=%s\n",
                    WiFi.localIP().toString().c_str(),
                    WiFi.RSSI(),
                    validName ? "valid" : "invalid");
    }
  } else {
    // blink red while disconnected
    if (now % 600 < 300) {
      if (!blinkState) {
        setLed(50, 0, 0);
        blinkState = true;
      }
    } else {
      if (blinkState) {
        setLed(0, 0, 0);
        blinkState = false;
      }
    }
  }
}