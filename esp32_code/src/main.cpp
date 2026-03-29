/*
 * Smart Industrial Safety & Equipment Monitoring System
 * ESP32 Firmware with WiFi WebSocket Server (Hotspot Mode)
 *
 * Sensors: DHT11, MQ-135, Flame, SW-420 Vibration
 * Outputs: Relay (JQC3F 0-5V DC) -> Cooling Fan, Buzzer, LED
 *
 * Data is served via WebSocket to the HTML dashboard on the same network.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <DHT.h>

// ============================================================
// CONFIGURATION — Edit these values before uploading
// ============================================================
const char* WIFI_SSID     = "TK";      // <-- Your ESP32 Hotspot Name
const char* WIFI_PASSWORD = "qwerty12345";  // <-- Your ESP32 Hotspot Password

// ============================================================
// PIN DEFINITIONS
// ============================================================
#define DHT_PIN         4    // DHT11 Data pin
#define MQ135_PIN       34   // MQ-135 Analog output (ADC1 channel)
#define FLAME_PIN       27   // Flame sensor Digital output
#define VIBRATION_PIN   26   // SW-420 Vibration sensor Digital output
#define LED_PIN         2    // Warning LED
#define BUZZER_PIN      15   // Active Buzzer
#define RELAY_PIN       18   // Relay module control pin

// ============================================================
// SENSOR & DHT SETUP
// ============================================================
#define DHT_TYPE DHT11
DHT dht(DHT_PIN, DHT_TYPE);

// ============================================================
// DEFAULT THRESHOLD VALUES (can be updated via WebSocket)
// ============================================================
float   TEMP_THRESHOLD      = 35.0;   // °C
float   HUMIDITY_THRESHOLD  = 80.0;   // %
int     GAS_THRESHOLD       = 1800;   // ADC raw (0-4095)
bool    FIRE_ALERT_ENABLED  = true;
bool    VIBRATION_ALERT_ENABLED = true;

// ============================================================
// SERVER & STATE
// ============================================================
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

unsigned long lastReadTime     = 0;
unsigned long lastBroadcast    = 0;
const unsigned long READ_INTERVAL      = 1000;  // ms between sensor reads
const unsigned long BROADCAST_INTERVAL = 500;   // ms between WebSocket broadcasts

// Sensor state
float    temperature    = 0.0;
float    humidity       = 0.0;
int      gasValue       = 0;
bool     fireDetected   = false;
bool     vibrationDetected = false;
bool     fanRunning     = false;
bool     buzzerActive   = false;

// Alarm state
bool     tempAlarm      = false;
bool     humAlarm       = false;
bool     gasAlarm       = false;
bool     fireAlarm      = false;
bool     vibAlarm       = false;

// ============================================================
// ALARM LOGIC
// ============================================================
void updateAlarms() {
  tempAlarm = (temperature > TEMP_THRESHOLD);
  humAlarm  = (humidity    > HUMIDITY_THRESHOLD);
  gasAlarm  = (gasValue    > GAS_THRESHOLD);
  fireAlarm = (FIRE_ALERT_ENABLED   && fireDetected);
  vibAlarm  = (VIBRATION_ALERT_ENABLED && vibrationDetected);

  bool anyAlarm = tempAlarm || humAlarm || gasAlarm || fireAlarm || vibAlarm;

  // Fan: turn on when temperature exceeds threshold OR gas alarm
  fanRunning = (tempAlarm || gasAlarm || fireAlarm);
  
  // UPDATED: Relay is set to ACTIVE HIGH based on your hardware
  digitalWrite(RELAY_PIN, fanRunning ? HIGH : LOW);

  // Buzzer & LED: on for any alarm
  buzzerActive = anyAlarm;
  digitalWrite(BUZZER_PIN, buzzerActive ? HIGH : LOW);
  digitalWrite(LED_PIN,    anyAlarm     ? HIGH : LOW);
}

// ============================================================
// SENSOR READS
// ============================================================
void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) temperature = t;
  if (!isnan(h)) humidity    = h;

  gasValue        = analogRead(MQ135_PIN);
  fireDetected    = (digitalRead(FLAME_PIN)    == LOW);  // Flame sensor LOW = fire
  vibrationDetected = (digitalRead(VIBRATION_PIN) == HIGH); // SW-420 HIGH = vibration
}

// ============================================================
// WEBSOCKET BROADCAST
// ============================================================
void broadcastData() {
  JsonDocument doc;
  
  // Basic State
  doc["temp"]      = round(temperature * 10.0) / 10.0;
  doc["hum"]       = round(humidity    * 10.0) / 10.0;
  doc["gas"]       = gasValue;
  doc["fire"]      = fireDetected;
  doc["vib"]       = vibrationDetected;
  doc["fan"]       = fanRunning;
  doc["buzzer"]    = buzzerActive;

  // Alarms (ArduinoJson 7 automatically creates the nested object)
  doc["alarms"]["temp"]  = tempAlarm;
  doc["alarms"]["hum"]   = humAlarm;
  doc["alarms"]["gas"]   = gasAlarm;
  doc["alarms"]["fire"]  = fireAlarm;
  doc["alarms"]["vib"]   = vibAlarm;

  // Thresholds (echo back so dashboard stays in sync)
  doc["thresholds"]["temp"]  = TEMP_THRESHOLD;
  doc["thresholds"]["hum"]   = HUMIDITY_THRESHOLD;
  doc["thresholds"]["gas"]   = GAS_THRESHOLD;

  String payload;
  serializeJson(doc, payload);
  ws.textAll(payload);
}

// ============================================================
// WEBSOCKET EVENT HANDLER
// ============================================================
void handleWebSocketMessage(void* arg, uint8_t* data, size_t len) {
  AwsFrameInfo* info = (AwsFrameInfo*)arg;
  
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
    JsonDocument doc;
    
    // Safer ArduinoJson 7 deserialization directly from the byte array buffer
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
      Serial.print("[WS] deserializeJson() failed: ");
      Serial.println(err.c_str());
      return;
    }

    if (doc.containsKey("setThreshold")) {
      JsonObject t = doc["setThreshold"];
      if (t.containsKey("temp"))  TEMP_THRESHOLD     = t["temp"].as<float>();
      if (t.containsKey("hum"))   HUMIDITY_THRESHOLD = t["hum"].as<float>();
      if (t.containsKey("gas"))   GAS_THRESHOLD      = t["gas"].as<int>();
    }
    
    if (doc.containsKey("fireAlertEnabled"))   FIRE_ALERT_ENABLED      = doc["fireAlertEnabled"].as<bool>();
    if (doc.containsKey("vibAlertEnabled"))    VIBRATION_ALERT_ENABLED = doc["vibAlertEnabled"].as<bool>();
  }
}

void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("[WS] Client #%u connected from %s\n",
                  client->id(), client->remoteIP().toString().c_str());
    broadcastData(); // Send current state immediately
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("[WS] Client #%u disconnected\n", client->id());
  } else if (type == WS_EVT_DATA) {
    handleWebSocketMessage(arg, data, len);
  } else if (type == WS_EVT_ERROR) {
    Serial.printf("[WS] Error(%u) %s\n", *((uint16_t*)arg), (char*)data);
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Industrial Safety Monitor - Booting ===");

  // Pin modes
  pinMode(FLAME_PIN,     INPUT);
  pinMode(VIBRATION_PIN, INPUT);
  pinMode(LED_PIN,       OUTPUT);
  pinMode(BUZZER_PIN,    OUTPUT);
  pinMode(RELAY_PIN,     OUTPUT);

  // UPDATED: Safe defaults: fan OFF (relay LOW for active-high hardware)
  digitalWrite(RELAY_PIN,  LOW);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN,    LOW);

  // DHT
  dht.begin();
  delay(2000); // Allow DHT11 to stabilise

  // WiFi - Create ESP32 Hotspot (Access Point)
  Serial.printf("[WiFi] Starting Hotspot named: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASSWORD); 
  delay(500); // Give it a moment to start
  
  Serial.println("[WiFi] Hotspot Started Successfully!");
  Serial.printf("[WiFi] Connect your laptop Wi-Fi to '%s'\n", WIFI_SSID);
  Serial.printf("[WiFi] ESP32 IP Address is: %s\n", WiFi.softAPIP().toString().c_str());

  // WebSocket
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // Serve root URL
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "text/plain",
      String("ESP32 Safety Monitor Online\nWebSocket: ws://") +
      WiFi.softAPIP().toString() + "/ws\n" +
      "Open dashboard_index.html in your browser."
    );
  });

  // CORS headers for local file access
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin",  "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

  server.begin();
  Serial.println("[Server] HTTP + WebSocket server started.");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  unsigned long now = millis();

  // Read sensors at READ_INTERVAL
  if (now - lastReadTime >= READ_INTERVAL) {
    lastReadTime = now;
    readSensors();
    updateAlarms();
  }

  // Broadcast at BROADCAST_INTERVAL
  if (now - lastBroadcast >= BROADCAST_INTERVAL) {
    lastBroadcast = now;
    if (ws.count() > 0) {
      broadcastData();
    }
  }

  ws.cleanupClients();
}