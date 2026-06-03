// ╔══════════════════════════════════════════════════════════════╗
// ║     ESP32 — NEXUS SECURITY — MQTT CLOUD EDITION             ║
// ║     Replaces local web server with HiveMQ cloud MQTT        ║
// ║     STM32 wiring and serial parsing unchanged               ║
// ╚══════════════════════════════════════════════════════════════╝

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ═══════════════════════════════════════════════════════════════
//  WIFI — your network
// ═══════════════════════════════════════════════════════════════
const char* WIFI_SSID     = "Omar";
const char* WIFI_PASSWORD = "Omar$2005$2005";

// ═══════════════════════════════════════════════════════════════
//  HIVEMQ CLOUD — fill in your cluster details
//  Cluster URL:  found in HiveMQ Cloud dashboard (no "ssl://")
//  Port:         8883 (TLS)
//  Username/pw:  created under Access Management
// ═══════════════════════════════════════════════════════════════
const char* MQTT_HOST     = "nexus-4408dcb8.a01.euc1.aws.hivemq.cloud";
const int   MQTT_PORT     = 8883;
const char* MQTT_USER     = "omarmarei";
const char* MQTT_PASS     = "Omar$2005";
const char* MQTT_CLIENT   = "nexus-esp32";

// ═══════════════════════════════════════════════════════════════
//  MQTT TOPICS
//  esp32/nexus/state  — ESP32 publishes full system state (JSON)
//  esp32/nexus/log    — ESP32 publishes log entries (string)
//  esp32/nexus/cmd    — Dashboard publishes commands to ESP32
// ═══════════════════════════════════════════════════════════════
#define TOPIC_STATE   "esp32/nexus/state"
#define TOPIC_LOG     "esp32/nexus/log"
#define TOPIC_CMD     "esp32/nexus/cmd"

// ═══════════════════════════════════════════════════════════════
//  UART TO STM32
// ═══════════════════════════════════════════════════════════════
#define RXD2 4
#define TXD2 2

// ═══════════════════════════════════════════════════════════════
//  RUNTIME STATE
// ═══════════════════════════════════════════════════════════════
bool systemArmed  = false;
bool pirTriggered = false;
bool doorOpen     = false;
bool alertActive  = false;

#define LOG_SIZE 20
String eventLog[LOG_SIZE];
int    logCount = 0;

unsigned long lastPublishTime  = 0;
const unsigned long PUBLISH_INTERVAL = 3000; // publish state every 3s

// ═══════════════════════════════════════════════════════════════
//  MQTT + TLS CLIENT
// ═══════════════════════════════════════════════════════════════
WiFiClientSecure tlsClient;
PubSubClient     mqtt(tlsClient);

// ═══════════════════════════════════════════════════════════════
//  HELPERS
// ═══════════════════════════════════════════════════════════════
String getTimestamp() {
  unsigned long s = millis() / 1000;
  unsigned long m = s / 60;
  unsigned long h = m / 60;
  char buf[12];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", h % 24, m % 60, s % 60);
  return String(buf);
}

void addLog(String msg) {
  for (int i = LOG_SIZE - 1; i > 0; i--) eventLog[i] = eventLog[i - 1];
  eventLog[0] = "[" + getTimestamp() + "] " + msg;
  if (logCount < LOG_SIZE) logCount++;
  // Publish log entry immediately so dashboard sees it fast
  if (mqtt.connected()) {
    mqtt.publish(TOPIC_LOG, eventLog[0].c_str(), true);
  }
}

void sendToSTM32(String cmd, int waitAfterMs = 0) {
  Serial2.println(cmd);
  Serial.println(">> STM32: " + cmd);
  if (waitAfterMs > 0) delay(waitAfterMs);
}

// ═══════════════════════════════════════════════════════════════
//  PUBLISH FULL STATE AS JSON
//  Dashboard subscribes to this topic and updates UI from it
// ═══════════════════════════════════════════════════════════════
void publishState() {
  // Build JSON log array (last 10 entries)
  StaticJsonDocument<1024> doc;
  doc["armed"] = systemArmed;
  doc["pir"]   = pirTriggered;
  doc["door"]  = doorOpen;
  doc["alarm"] = alertActive;
  doc["uptime"] = millis() / 1000;

  JsonArray log = doc.createNestedArray("log");
  int count = min(logCount, 10);
  for (int i = 0; i < count; i++) {
    log.add(eventLog[i]);
  }

  char buf[1024];
  serializeJson(doc, buf);
  mqtt.publish(TOPIC_STATE, buf, true); // retained — new subscribers get last state instantly
}

// ═══════════════════════════════════════════════════════════════
//  HANDLE COMMANDS FROM DASHBOARD via MQTT
//  Dashboard publishes to esp32/nexus/cmd
//  Commands: ARM | DISARM | TESTSMS | PHONE=+20... | PIN=1234:5678
// ═══════════════════════════════════════════════════════════════
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String cmd = "";
  for (unsigned int i = 0; i < length; i++) cmd += (char)payload[i];
  cmd.trim();
  Serial.println("MQTT CMD: " + cmd);

  if (cmd == "ARM") {
    sendToSTM32("CMD:ARM");
    systemArmed = true;
    addLog("Armed via dashboard");
    publishState();
  }
  else if (cmd == "DISARM") {
    sendToSTM32("CMD:DISARM");
    systemArmed  = false;
    alertActive  = false;
    addLog("Disarmed via dashboard");
    publishState();
  }
  else if (cmd == "TESTSMS") {
    sendToSTM32("CMD:TESTSMS");
    addLog("Test SMS requested");
  }
  else if (cmd.startsWith("PHONE=")) {
    String phone = cmd.substring(6);
    phone.trim();
    if (phone.length() >= 10) {
      sendToSTM32("CMD:PHONE=" + phone, 400);
      addLog("Phone updated: " + phone);
    }
  }
  else if (cmd.startsWith("PIN=")) {
    String payload2 = cmd.substring(4);
    int sep = payload2.indexOf(':');
    if (sep != -1) {
      String armPin = payload2.substring(0, sep);
      String disPin = payload2.substring(sep + 1);
      armPin.trim(); disPin.trim();
      sendToSTM32("CMD:PIN=" + armPin + ":" + disPin, 400);
      addLog("PIN codes updated");
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  PARSE MESSAGES FROM STM32 (unchanged from original)
// ═══════════════════════════════════════════════════════════════
void parseSTM32Message(String msg) {
  msg.trim();
  if (msg.length() == 0) return;
  Serial.println("<< STM32: [" + msg + "]");

  if      (msg == "ARMED")    { systemArmed = true;  addLog("System armed"); }
  else if (msg == "DISARMED") { systemArmed = false;  alertActive = false; addLog("System disarmed"); }
  else if (msg == "PIR:1")    { pirTriggered = true;  if (systemArmed) { alertActive = true; addLog("PIR sensor triggered!"); } }
  else if (msg == "PIR:0")    { pirTriggered = false; }
  else if (msg == "DOOR:1")   { doorOpen = true;      if (systemArmed) { alertActive = true; addLog("Door opened!"); } }
  else if (msg == "DOOR:0")   { doorOpen = false; }
  else if (msg == "ALARM")    { alertActive = true;   addLog("ALARM triggered!"); }
  else if (msg.startsWith("LOG:")) { addLog(msg.substring(4)); }

  publishState(); // push update immediately on any STM32 event
}

// ═══════════════════════════════════════════════════════════════
//  MQTT RECONNECT
// ═══════════════════════════════════════════════════════════════
void mqttReconnect() {
  int attempts = 0;
  while (!mqtt.connected() && attempts < 5) {
    Serial.print("Connecting to MQTT...");
    if (mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS)) {
      Serial.println(" connected!");
      mqtt.subscribe(TOPIC_CMD);
      addLog("MQTT connected");
      publishState();
    } else {
      Serial.printf(" failed (rc=%d), retrying in 5s\n", mqtt.state());
      delay(5000);
      attempts++;
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);

  // Connect WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());

  // TLS — HiveMQ public CA is trusted; skip full cert verification
  // For production, load the full CA cert instead
  tlsClient.setInsecure();

  // MQTT setup
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(60);

  mqttReconnect();
  addLog("WiFi: " + WiFi.localIP().toString());
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
  // Maintain MQTT connection
  if (!mqtt.connected()) mqttReconnect();
  mqtt.loop();

  // Read from STM32
  while (Serial2.available()) {
    String line = Serial2.readStringUntil('\n');
    parseSTM32Message(line);
  }

  // Periodic state publish (heartbeat)
  if (millis() - lastPublishTime > PUBLISH_INTERVAL) {
    lastPublishTime = millis();
    publishState();
  }

  if (alertActive && !systemArmed) alertActive = false;
}