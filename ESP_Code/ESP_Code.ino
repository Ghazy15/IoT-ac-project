#include <WiFi.h>
#include <WiFiManager.h> 
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

// ================= USER CONFIG =================
String MODEL_NAME = "AC_Node0"; 

const char* MQTT_HOST = "broker.hivemq.com"; 
const int   MQTT_PORT = 1883;

// ⚠️ MUST MATCH SERVER.JS
String TOPIC_PREFIX = "/ac_project_unique_123/"; 

String NODE_ID = "AC_" + String((uint32_t)ESP.getEfuseMac(), HEX);

// PINS
#define IR_RX_PIN 34
#define IR_TX_PIN 25

// SETTINGS
#define MIN_TEMP 16
#define MAX_TEMP 30

// --- IR SETTINGS ---
const uint16_t kCaptureBufferSize = 1024; 
const uint8_t  kTimeout = 50;             
const uint16_t kFrequency = 38;           

// OBJECTS
WiFiClient espClient;
PubSubClient mqtt(espClient);
WiFiUDP udp;
IPAddress fileServerIP;

IRrecv irrecv(IR_RX_PIN, kCaptureBufferSize, kTimeout, true);
IRsend irsend(IR_TX_PIN);
decode_results results;

// STATE
bool acPower = false;
int acTemp = 24; 
unsigned long lastHeartbeat = 0;
unsigned long lastLoopDebug = 0;

/* ---------------- LOCAL STORAGE ---------------- */
void saveIR(String name, uint16_t* raw, uint16_t len) {
  DynamicJsonDocument doc(16384);
  doc["len"] = len;
  JsonArray arr = doc.createNestedArray("raw");
  for (uint16_t i = 0; i < len; i++) arr.add(raw[i]);
  File f = LittleFS.open("/codes/" + name + ".json", "w");
  if (f) { serializeJson(doc, f); f.close(); }
}

bool loadIR(String name, uint16_t*& raw, uint16_t& len) {
  String path = "/codes/" + name + ".json";
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  DynamicJsonDocument doc(16384);
  deserializeJson(doc, f); f.close();
  len = doc["len"];
  JsonArray arr = doc["raw"];
  raw = new uint16_t[len];
  for (uint16_t i = 0; i < len; i++) raw[i] = arr[i];
  return true;
}

/* ---------------- NETWORK SYNC ---------------- */
void uploadSignal(String name, uint16_t* raw, uint16_t len) {
  if (fileServerIP[0] == 0) return;
  HTTPClient http;
  String url = "http://" + fileServerIP.toString() + ":3000/api/profile/upload";
  DynamicJsonDocument doc(16384);
  doc["modelName"] = MODEL_NAME;
  doc["signalName"] = name;
  JsonObject data = doc.createNestedObject("data");
  data["len"] = len;
  JsonArray arr = data.createNestedArray("raw");
  for(int i=0; i<len; i++) arr.add(raw[i]);
  String json; serializeJson(doc, json);
  http.begin(url); http.addHeader("Content-Type", "application/json"); http.POST(json); http.end();
}

void downloadSignal(String name) {
  if (fileServerIP[0] == 0) return;
  HTTPClient http;
  String url = "http://" + fileServerIP.toString() + ":3000/api/profile/download?modelName=" + MODEL_NAME + "&signalName=" + name;
  http.begin(url);
  if(http.GET() == 200) {
    File f = LittleFS.open("/codes/" + name + ".json", "w");
    http.writeToStream(&f); f.close();
  }
  http.end();
}

void syncAll() {
  HTTPClient http;
  http.begin("http://" + fileServerIP.toString() + ":3000/api/profile/list?modelName=" + MODEL_NAME);
  if(http.GET() == 200) {
    DynamicJsonDocument doc(4096);
    deserializeJson(doc, http.getString());
    JsonArray signals = doc["signals"];
    for(String s : signals) downloadSignal(s);
  }
  http.end();
}

/* ---------------- LOGIC ---------------- */
void publishState() {
  char msg[128]; 
  sprintf(msg, "{\"power\":%s,\"temp\":%d}", acPower?"true":"false", acTemp);
  mqtt.publish((TOPIC_PREFIX + NODE_ID + "/state").c_str(), msg, true);
}

void updateState(String cmd) {
  Serial.println("[STATE] Logic Update: " + cmd);
  if (cmd == "on") acPower = true;
  else if (cmd == "off") acPower = false;
  else if (cmd == "power") acPower = !acPower;
  else if (cmd.startsWith("temp_") && cmd != "temp_up" && cmd != "temp_down") {
    acPower = true;
    acTemp = cmd.substring(5).toInt();
  }
  else if (cmd == "temp_up") {
    acPower = true;
    if (acTemp < MAX_TEMP) acTemp++;
  }
  else if (cmd == "temp_down") {
    acPower = true;
    if (acTemp > MIN_TEMP) acTemp--;
  }
  publishState();
}

// 📡 MATCHING LOGIC
bool matchProtocol(uint16_t* incoming, uint16_t len1, uint16_t* saved, uint16_t len2) {
  if (abs(len1 - len2) > 10) return false; 
  for (uint16_t i = 0; i < len1 && i < len2; i++) {
    uint16_t diff = abs(incoming[i] - saved[i]);
    uint16_t tolerance = saved[i] * 0.35; // Increased tolerance to 35%
    if (diff > tolerance) return false; 
  }
  return true;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg; for(int i=0;i<length;i++) msg += (char)payload[i];
  
  if (String(topic).endsWith("global/scan")) {
    publishState();
    mqtt.publish((TOPIC_PREFIX + "discovery").c_str(), ("{\"node\":\"" + NODE_ID + "\",\"model\":\"" + MODEL_NAME + "\"}").c_str(), true);
    return;
  }

  StaticJsonDocument<512> doc; deserializeJson(doc, msg);
  String action = doc["action"];
  String name = doc["name"];

  if (action == "learn") {
    Serial.println("[LEARN] Start Learning: " + name);
    unsigned long start = millis();
    while (millis() - start < 10000) { 
      mqtt.loop();
      if (irrecv.decode(&results)) {
        if (results.rawlen > 40) { 
          uint16_t* raw = resultToRawArray(&results);
          uint16_t len = results.rawlen - 1;
          saveIR(name, raw, len);       
          uploadSignal(name, raw, len); 
          delete[] raw;
          mqtt.publish((TOPIC_PREFIX + NODE_ID + "/event").c_str(), "{\"event\":\"learned\"}");
          Serial.println("[LEARN] Success!");
          
          // ⚠️ IMPORTANT: Restart Receiver
          irrecv.enableIRIn(); 
          break;
        }
        irrecv.resume();
      }
      yield();
    }
  } 
  else if (action == "send") {
    String fileToSend = name;
    if (!LittleFS.exists("/codes/" + fileToSend + ".json")) downloadSignal(fileToSend);
    
    uint16_t* raw; uint16_t len;
    
    if (loadIR(fileToSend, raw, len)) {
      Serial.println("[SEND] Sending: " + name);
      
      // ⚠️ STOP LISTENING BEFORE SENDING
      irrecv.disableIRIn(); 
      delay(10);

      irsend.sendRaw(raw, len, kFrequency);
      
      // ⚠️ RESTART LISTENER AFTER SENDING
      delay(10);
      irrecv.enableIRIn(); 
      Serial.println("[SEND] Receiver Restarted");
      
      delete[] raw;
      updateState(name);
    }
  } 
  else if (action == "sync") {
    syncAll();
  }
}

void setup() {
  Serial.begin(115200);
  LittleFS.begin(true);
  if (!LittleFS.exists("/codes")) LittleFS.mkdir("/codes");
  
  // ⚠️ CRITICAL ORDER: Start Sender FIRST, then Receiver
  irsend.begin();
  delay(50);
  irrecv.enableIRIn(); 
  Serial.println("[SYSTEM] IR Subsystem Initialized");

  WiFiManager wm; wm.setTimeout(180);
  if(!wm.autoConnect("AC_SETUP")) ESP.restart();

  udp.begin(9999);
  unsigned long start = millis();
  while(fileServerIP[0] == 0 && millis() - start < 5000) {
    udp.beginPacket(IPAddress(255,255,255,255), 9999);
    udp.print("DISCOVER_AC_SERVER"); udp.endPacket();
    delay(500);
    if (udp.parsePacket()) {
      char buf[32]; udp.read(buf, 32);
      if (strstr(buf, "AC_SERVER_HERE")) fileServerIP = udp.remoteIP();
    }
  }

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
}

void loop() {
  // 1. Connection Logic
  if (!mqtt.connected()) {
    unsigned long now = millis();
    static unsigned long lastReconnectAttempt = 0;
    if (now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      if (mqtt.connect(NODE_ID.c_str())) { 
        mqtt.subscribe((TOPIC_PREFIX + NODE_ID + "/cmd").c_str());
        mqtt.subscribe((TOPIC_PREFIX + "global/scan").c_str());
        mqtt.publish((TOPIC_PREFIX + "discovery").c_str(), ("{\"node\":\"" + NODE_ID + "\",\"model\":\"" + MODEL_NAME + "\"}").c_str(), true);
        publishState();
      }
    }
  }
  mqtt.loop();

  // 2. Debug Pulse (Checks if loop is frozen)
  if (millis() - lastLoopDebug > 5000) {
    lastLoopDebug = millis();
    // Serial.println("[SYSTEM] Loop is running..."); // Uncomment if needed
  }

  // 3. Heartbeat
  if (millis() - lastHeartbeat > 15000) {
    lastHeartbeat = millis();
    if(mqtt.connected()) publishState(); 
  }

  // 4. IR LISTENER
  if (irrecv.decode(&results)) {
    // Only care about long signals (AC commands)
    if (results.rawlen > 40) {
      Serial.print("[IR] RAW SIGNAL DETECTED. Len: ");
      Serial.println(results.rawlen - 1);

      uint16_t* incomingRaw = resultToRawArray(&results);
      uint16_t incomingLen = results.rawlen - 1;

      File root = LittleFS.open("/codes");
      File f = root.openNextFile();
      bool match = false;

      while(f) {
        String fname = f.name();
        if(fname.endsWith(".json")) {
          String cleanName = fname;
          cleanName.replace("/codes/", ""); cleanName.replace(".json", "");
          
          uint16_t* fileRaw; uint16_t fileLen;
          if(loadIR(cleanName, fileRaw, fileLen)) {
            if(matchProtocol(incomingRaw, incomingLen, fileRaw, fileLen)) {
              Serial.println("[MATCH] ✅ Signal Matches: " + cleanName);
              updateState(cleanName);
              delete[] fileRaw;
              match = true;
              break;
            }
            delete[] fileRaw;
          }
        }
        f = root.openNextFile();
      }
      if (!match) Serial.println("[MATCH] ❌ No saved signal matched.");
      
      delete[] incomingRaw; 
    }
    
    // ⚠️ ESSENTIAL: Resume listening for next signal
    irrecv.resume();
  }
}