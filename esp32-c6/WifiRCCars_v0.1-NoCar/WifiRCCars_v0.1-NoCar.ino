#include <WiFi.h>
#include <WebSocketsServer.h> //v2.16.1 (WebSockets_Generic by Markus Sattler)
#include "WiFi_Credentials.h"

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;

WebSocketsServer webSocket = WebSocketsServer(81);

float leftX = 0.0;
float leftY = 0.0;
float rightX = 0.0;
float rightY = 0.0;
int btnA = 0;
int btnB = 0;

void handleMessage(uint8_t num, uint8_t * payload, size_t length) {
  String msg = String((char*)payload);

  // Very simple CSV format:
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
    leftX = vals[0];
    leftY = vals[1];
    rightX = vals[2];
    rightY = vals[3];
    btnA = (int)vals[4];
    btnB = (int)vals[5];

    Serial.print("LX: "); Serial.print(leftX, 3);
    Serial.print("  LY: "); Serial.print(leftY, 3);
    Serial.print("  RX: "); Serial.print(rightX, 3);
    Serial.print("  RY: "); Serial.print(rightY, 3);
    Serial.print("  A: "); Serial.print(btnA);
    Serial.print("  B: "); Serial.println(btnB);

    digitalWrite(LED_BUILTIN, btnA ? HIGH : LOW);
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
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

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