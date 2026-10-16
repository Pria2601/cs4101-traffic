/*
 * CS4101 Smart Traffic Signal - NodeMCU ESP8266 (WiFi gateway)
 * ------------------------------------------------------------
 * Job of this board (no traffic logic here):
 *   1. Read telemetry JSON lines from the Mega (SoftwareSerial, D6 = RX)
 *   2. Add the device name and publish to AWS IoT Core  -> topic traffic/pub
 *   3. Receive cloud commands from AWS IoT Core        <- topic traffic/sub
 *   4. Convert them to short text commands and send to the Mega (D5 = TX)
 *
 * Libraries (Arduino IDE > Library Manager):
 *   - PubSubClient by Nick O'Leary
 *   - ArduinoJson by Benoit Blanchon (version 7.x)
 * Board: "NodeMCU 1.0 (ESP-12E Module)" from the ESP8266 boards package.
 * Fill in secrets.h before uploading.
 */

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <SoftwareSerial.h>
#include <time.h>
#include "secrets.h"

const char* TOPIC_PUB = "traffic/pub";   // telemetry to cloud
const char* TOPIC_SUB = "traffic/sub";   // commands from cloud
const char* TOPIC_ACK = "traffic/ack";   // Mega's replies to commands

const uint8_t MEGA_RX_PIN = D6;  // from Mega TX1 (through voltage divider)
const uint8_t MEGA_TX_PIN = D5;  // to Mega RX1 (pin 19)

SoftwareSerial megaSerial;

BearSSL::X509List   caCert(AWS_CERT_CA);
BearSSL::X509List   clientCert(AWS_CERT_CRT);
BearSSL::PrivateKey clientKey(AWS_CERT_PRIVATE);

WiFiClientSecure net;
PubSubClient     mqtt(net);

char     megaLine[768];
uint16_t megaLen = 0;
unsigned long lastConnectTry = 0;
unsigned long published = 0, dropped = 0;

// --------------------------------------------------------------------
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("[WIFI] Connecting to "); Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WIFI] Connected, IP "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("[WIFI] Failed, will retry");
  }
}

// TLS certificates need the correct time
void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("[TIME] Syncing");
  time_t now = time(nullptr);
  unsigned long start = millis();
  while (now < 1700000000 && millis() - start < 20000) {
    delay(500);
    Serial.print('.');
    now = time(nullptr);
  }
  Serial.println(now >= 1700000000 ? " done" : " failed");
}

bool connectAWS() {
  Serial.print("[AWS] Connecting to IoT Core as "); Serial.println(DEVICE_ID);
  if (!mqtt.connect(DEVICE_ID)) {
    Serial.print("[AWS] Failed, state="); Serial.println(mqtt.state());
    return false;
  }
  mqtt.subscribe(TOPIC_SUB);
  Serial.println("[AWS] Connected, subscribed to traffic/sub");
  return true;
}

// --------------------------------------------------------------------
// Cloud -> Mega
// Accepted JSON on traffic/sub:
//   {"cmd":"mode","mode":"AUTO"}
//   {"cmd":"mode","mode":"MANUAL","lane":2}
//   {"cmd":"mode","mode":"ALLRED"}
//   {"cmd":"set","key":"MAXGREEN","value":25}
//   {"cmd":"reset"}   {"cmd":"ping"}
// --------------------------------------------------------------------
void onCloudMessage(char* topic, byte* payload, unsigned int length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) {
    Serial.println("[AWS->MEGA] Ignored: not valid JSON");
    return;
  }

  String cmd  = doc["cmd"] | "";
  String text = "";
  cmd.toLowerCase();

  if (cmd == "mode") {
    String m = doc["mode"] | "";
    m.toUpperCase();
    if (m == "AUTO" || m == "ALLRED") text = "MODE " + m;
    else if (m == "MANUAL") {
      int lane = doc["lane"] | 0;
      if (lane >= 1 && lane <= 4) text = "MODE MANUAL " + String(lane);
    }
  } else if (cmd == "set") {
    String key = doc["key"] | "";
    key.toUpperCase();
    if (key.length() > 0 && !doc["value"].isNull())
      text = "SET " + key + " " + String(doc["value"].as<float>(), 1);
  } else if (cmd == "reset") {
    text = "RESET";
  } else if (cmd == "ping") {
    text = "PING";
  }

  if (text.length() == 0) {
    Serial.println("[AWS->MEGA] Ignored: unknown command");
    return;
  }
  megaSerial.print(text);
  megaSerial.print('\n');
  Serial.print("[AWS->MEGA] "); Serial.println(text);
}

// --------------------------------------------------------------------
// Mega -> Cloud
// --------------------------------------------------------------------
void handleMegaLine(char* lineText) {
  JsonDocument doc;
  if (deserializeJson(doc, lineText)) {
    Serial.print("[MEGA] Not JSON, skipped: "); Serial.println(lineText);
    return;
  }
  doc["device"] = DEVICE_ID;

  const char* topic = doc["ack"].isNull() ? TOPIC_PUB : TOPIC_ACK;
  char out[900];
  size_t n = serializeJson(doc, out, sizeof(out));

  Serial.print("[MEGA->AWS] "); Serial.println(out);
  if (mqtt.connected() && mqtt.publish(topic, out, n)) {
    published++;
  } else {
    dropped++;
    Serial.println("[MEGA->AWS] Not published (offline)");
  }
}

void readMega() {
  while (megaSerial.available()) {
    char c = megaSerial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      megaLine[megaLen] = 0;
      if (megaLen > 0) handleMegaLine(megaLine);
      megaLen = 0;
    } else if (megaLen < sizeof(megaLine) - 1) {
      megaLine[megaLen++] = c;
    } else {
      megaLen = 0;  // line too long, drop it
    }
  }
}

// --------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  // RX buffer 512 so nothing is lost while TLS/MQTT is busy
  megaSerial.begin(9600, SWSERIAL_8N1, MEGA_RX_PIN, MEGA_TX_PIN, false, 512);
  Serial.println("\n=== CS4101 Traffic Gateway (NodeMCU) ===");

  connectWiFi();
  syncTime();

  net.setTrustAnchors(&caCert);
  net.setClientRSACert(&clientCert, &clientKey);
  mqtt.setServer(AWS_IOT_ENDPOINT, 8883);
  mqtt.setBufferSize(1024);
  mqtt.setCallback(onCloudMessage);
  connectAWS();
}

void loop() {
  readMega();

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastConnectTry > 5000) { lastConnectTry = millis(); connectWiFi(); }
  } else if (!mqtt.connected()) {
    if (millis() - lastConnectTry > 5000) { lastConnectTry = millis(); connectAWS(); }
  } else {
    mqtt.loop();
  }

  readMega();
}
