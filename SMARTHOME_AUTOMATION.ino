/*********************************************************************************
 * Professional Home Automation Project: 4-Light ESP8266 Controller
 * Description:
 * This code runs on an ESP8266 to control four light bulbs via active-low 
 * relays. It connects to a public MQTT broker, includes a timer, and features
 * a flame sensor for safety shutdown and automatic web-based alarms.
 * Now with IR remote control functionality.
 *
 * Author: Arnab Mandal
 * Version: 5.3 (Based on V5.2 with IR Remote Control)
 *********************************************************************************/

// --- LIBRARIES ---
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>

// --- EEPROM CONFIGURATION ---
#define EEPROM_SIZE 512
#define SSID_ADDR 0
#define PASS_ADDR 50
#define DEVICE_NAME_ADDR 100
#define SETUP_FLAG_ADDR 150

// --- WIFI CREDENTIALS (Only used as fallback) ---
String savedSSID = "";
String savedPassword = "";

// --- MQTT BROKER CONFIGURATION ---
const char* mqtt_server = "broker.hivemq.com"; // Using a reliable public broker
const int mqtt_port = 1883; // Standard MQTT port (TCP)
const char* clientId = "ESP8266_MultiLight_Client_V5_SimpleAlarm"; // Unique client ID

// --- MQTT TOPICS ---
const char* light_set_topic_wildcard = "homeautomation/project/light/+/set";
const char* light_get_status_topic_wildcard = "homeautomation/project/light/+/getStatus";
const char* light_log_topic = "homeautomation/project/light/log";
const char* light_timer_set_topic = "homeautomation/project/light/setTimer";
const char* light_timer_clear_topic = "homeautomation/project/light/clearTimer";
const char* esp_status_topic = "homeautomation/project/esp/status";
const char* esp_get_status_topic = "homeautomation/project/esp/getStatus";
const char* flame_alarm_topic = "homeautomation/project/alarm/flame"; // New topic for flame alarm
const char* esp_wifi_signal_topic = "homeautomation/project/esp/wifi_signal"; // WiFi signal strength topic

// --- HARDWARE CONFIGURATION ---
const int relayPins[] = { D1, D2, D3, D4 };
const int NUM_LIGHTS = sizeof(relayPins) / sizeof(int);
const int flameSensorPin = D5; // Flame sensor digital output (D0) is connected to D5
const int irReceiverPin = D6; // IR receiver is connected to D6

// --- IR REMOTE CONFIGURATION ---
IRrecv irrecv(irReceiverPin);
decode_results results;
const uint64_t TOGGLE_CODE = 0xB27BE0; // IR code to toggle all lights

// --- NTP (TIME) CLIENT SETUP ---
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000); // IST Offset

// --- GLOBAL VARIABLES ---
WiFiClient espClient;
PubSubClient client(espClient);
ESP8266WebServer server(80);
DNSServer dnsServer;
bool lightStates[NUM_LIGHTS] = {false};
char msg[128];
bool isFlameDetected = false; // State variable for the flame sensor
bool isConfigMode = false;
bool deviceConfigured = false;
String chipID;
String deviceName = "SmartHome";

// Timer variables
int onTimeHour = -1, onTimeMinute = -1;
int offTimeHour = -1, offTimeMinute = -1;
bool isTimerActive = false;
int lastMinuteChecked = -1;

// Status publish timer
unsigned long lastStatusPublishTime = 0;
const long statusPublishInterval = 30000; // 30 seconds

// WiFi signal publish timer
unsigned long lastWiFiSignalPublishTime = 0;
const long wifiSignalPublishInterval = 5000; // 5 seconds

// --- FUNCTION PROTOTYPES ---
void setup_wifi();
void reconnect();
void callback(char* topic, byte* payload, unsigned int length);
void setLightState(int lightIndex, bool state, String source);
void updateLightStatus(int lightNumber, bool state);
void publishLog(const char* logMessage);
void setTimer(String payload);
void clearTimer();
void checkTimer();
void handleFlameSensor();
void handleIR(); // New function for IR remote
// Captive Portal Functions
void startAPMode();
bool connectToWiFi(const String &ssid, const String &password);
void handleRoot();
void handleScan();
void handleSave();
void writeStringToEEPROM(int addr, const String &data);
String readStringFromEEPROM(int addr, int maxLen);
bool isDeviceConfigured();
void setDeviceConfigured();
void clearEEPROM();

// --- INITIAL SETUP ---
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n========================================");
  Serial.println("  Smart Home Automation System v5.4");
  Serial.println("  With WiFi Captive Portal");
  Serial.println("========================================");
  
  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // Get chip ID
  chipID = String(ESP.getChipId(), HEX);
  chipID.toUpperCase();
  Serial.println("Chip ID: " + chipID);
  
  // Initialize hardware pins
  for (int i = 0; i < NUM_LIGHTS; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
  }

  pinMode(flameSensorPin, INPUT_PULLUP);
  irrecv.enableIRIn();
  
  // Check if device is configured
  deviceConfigured = isDeviceConfigured();
  
  if (deviceConfigured) {
    // Load saved credentials
    savedSSID = readStringFromEEPROM(SSID_ADDR, 50);
    savedPassword = readStringFromEEPROM(PASS_ADDR, 50);
    deviceName = readStringFromEEPROM(DEVICE_NAME_ADDR, 50);
    
    if (deviceName.length() == 0) {
      deviceName = "Smart Home Device";
    }
    
    Serial.println("Device configured: YES");
    Serial.println("Device Name: " + deviceName);
    Serial.println("Connecting to: " + savedSSID);
    
    // Try to connect to saved WiFi
    if (connectToWiFi(savedSSID, savedPassword)) {
      // WiFi connected successfully
      client.setServer(mqtt_server, mqtt_port);
      client.setCallback(callback);
      timeClient.begin();
      
      Serial.println("System ready!");
    } else {
      // Failed to connect - start AP mode
      Serial.println("Failed to connect. Starting setup mode...");
      startAPMode();
    }
  } else {
    // Not configured - start AP mode
    Serial.println("Device not configured. Starting setup mode...");
    startAPMode();
  }
  
  Serial.println("========================================\n");
}

// --- MAIN LOOP ---
void loop() {
  if (isConfigMode) {
    // AP Mode - Handle captive portal
    dnsServer.processNextRequest();
    server.handleClient();
  } else {
    // Normal Mode - Handle MQTT and sensors
    if (!client.connected()) {
      reconnect();
    }
    client.loop();
    
    handleFlameSensor(); // Check the flame sensor continuously
    handleIR(); // Check for IR signals

    timeClient.update();
    int currentMinute = timeClient.getMinutes();
    if (currentMinute != lastMinuteChecked) {
      checkTimer();
      lastMinuteChecked = currentMinute;
    }

    // Periodically publish ESP status
    unsigned long currentMillis = millis();
    if (currentMillis - lastStatusPublishTime > statusPublishInterval) {
      lastStatusPublishTime = currentMillis;
      if (client.connected()) {
        client.publish(esp_status_topic, "online", true);
      }
    }
    
    // Periodically publish WiFi signal strength
    if (currentMillis - lastWiFiSignalPublishTime > wifiSignalPublishInterval) {
      lastWiFiSignalPublishTime = currentMillis;
      if (client.connected() && WiFi.status() == WL_CONNECTED) {
        int rssi = WiFi.RSSI();
        char rssiStr[8];
        snprintf(rssiStr, sizeof(rssiStr), "%d", rssi);
        client.publish(esp_wifi_signal_topic, rssiStr, false);
      }
    }
  }

  delay(10);
}

// --- SENSOR HANDLING (NEW FUNCTION) ---
void handleFlameSensor() {
  int flameState = digitalRead(flameSensorPin);

  // The sensor's digital pin (D0) goes LOW when flame is detected.
  if (flameState == LOW && !isFlameDetected) {
    isFlameDetected = true;
    publishLog("CRITICAL: Flame Detected! Shutting down all appliances.");
    client.publish(flame_alarm_topic, "DETECTED", true);

    for (int i = 0; i < NUM_LIGHTS; i++) {
      setLightState(i, false, "Flame Safety Shutdown");
    }
  } else if (flameState == HIGH && isFlameDetected) {
    isFlameDetected = false;
    publishLog("INFO: Flame alarm cleared.");
    client.publish(flame_alarm_topic, "CLEAR", true);
  }
}

// --- IRREMOTE HANDLING (NEW FUNCTION) ---
void handleIR() {
  if (irrecv.decode(&results)) {
    if (results.value == TOGGLE_CODE) {
      bool newState = !lightStates[0]; // Toggle based on the first light's state
      String source = "IR Remote";
      for (int i = 0; i < NUM_LIGHTS; i++) {
        setLightState(i, newState, source);
      }
    }
    irrecv.resume(); // Receive the next value
  }
}

// --- EEPROM FUNCTIONS ---
void writeStringToEEPROM(int addr, const String &data) {
  int len = data.length();
  for (int i = 0; i < len; i++) {
    EEPROM.write(addr + i, data[i]);
  }
  EEPROM.write(addr + len, '\0');
}

String readStringFromEEPROM(int addr, int maxLen = 50) {
  String data = "";
  char c = EEPROM.read(addr);
  int i = 0;
  while (c != '\0' && i < maxLen) {
    data += c;
    i++;
    c = EEPROM.read(addr + i);
  }
  return data;
}

bool isDeviceConfigured() {
  byte flag = EEPROM.read(SETUP_FLAG_ADDR);
  return (flag == 0xAA);
}

void setDeviceConfigured() {
  EEPROM.write(SETUP_FLAG_ADDR, 0xAA);
  EEPROM.commit();
}

void clearEEPROM() {
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
}

// --- WIFI FUNCTIONS ---
bool connectToWiFi(const String &ssid, const String &password) {
  Serial.println("Connecting to WiFi: " + ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("✓ WiFi Connected!");
    Serial.println("IP Address: " + WiFi.localIP().toString());
    return true;
  } else {
    Serial.println("✗ WiFi Connection Failed!");
    return false;
  }
}

void startAPMode() {
  Serial.println("Starting Access Point Mode...");
  WiFi.mode(WIFI_AP);
  
  String apName = "SmartHome-" + chipID;
  WiFi.softAP(apName.c_str());
  
  IPAddress apIP = WiFi.softAPIP();
  Serial.println("AP Started: " + apName);
  Serial.println("IP: " + apIP.toString());
  
  // Start DNS server for captive portal
  dnsServer.start(53, "*", apIP);
  
  // Setup web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);
  
  server.begin();
  isConfigMode = true;
}

void ICACHE_FLASH_ATTR reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (client.connect(clientId, NULL, NULL, esp_status_topic, 1, true, "offline")) {
      Serial.println("connected");
      client.subscribe(light_set_topic_wildcard);
      client.subscribe(light_get_status_topic_wildcard);
      client.subscribe(light_timer_set_topic);
      client.subscribe(light_timer_clear_topic);
      client.subscribe(esp_get_status_topic);
      client.publish(esp_status_topic, "online", true);
      publishLog("ESP8266 connected. Syncing all light statuses.");
      for(int i = 0; i < NUM_LIGHTS; i++) {
        updateLightStatus(i + 1, lightStates[i]);
      }
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void ICACHE_FLASH_ATTR callback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);
  String messageStr;
  for (unsigned int i = 0; i < length; i++) {
    messageStr += (char)payload[i];
  }
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  Serial.println(messageStr);

  if (topicStr == light_timer_set_topic) {
    setTimer(messageStr);
    return;
  }
  if (topicStr == light_timer_clear_topic) {
    clearTimer();
    return;
  }
  if (topicStr == esp_get_status_topic) {
    client.publish(esp_status_topic, "online", true);
    return;
  }
  String topicPart = topicStr;
  topicPart.remove(0, strlen("homeautomation/project/light/"));
  int lightNumber = topicPart.toInt();
  if (lightNumber < 1 || lightNumber > NUM_LIGHTS) return;
  int lightIndex = lightNumber - 1;
  if (topicStr.endsWith("/set")) {
    setLightState(lightIndex, messageStr == "ON", "user command");
  } else if (topicStr.endsWith("/getStatus")) {
    updateLightStatus(lightNumber, lightStates[lightIndex]);
  }
}

// --- HELPER FUNCTIONS ---
void ICACHE_FLASH_ATTR setLightState(int lightIndex, bool state, String source) {
  digitalWrite(relayPins[lightIndex], state ? HIGH : LOW);
  lightStates[lightIndex] = state;
  snprintf(msg, sizeof(msg), "Light %d turned %s by %s.", lightIndex + 1, state ? "ON" : "OFF", source.c_str());
  publishLog(msg);
  updateLightStatus(lightIndex + 1, state);
}

void ICACHE_FLASH_ATTR updateLightStatus(int lightNumber, bool state) {
  char topic[50];
  snprintf(topic, sizeof(topic), "homeautomation/project/light/%d/status", lightNumber);
  client.publish(topic, state ? "ON" : "OFF", true);
}

void ICACHE_FLASH_ATTR publishLog(const char* logMessage) {
  client.publish(light_log_topic, logMessage);
}

// --- TIMER FUNCTIONS ---
void ICACHE_FLASH_ATTR setTimer(String payload) {
  StaticJsonDocument<128> doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    publishLog("Error: Failed to parse timer settings.");
    return;
  }
  sscanf(doc["on"], "%d:%d", &onTimeHour, &onTimeMinute);
  sscanf(doc["off"], "%d:%d", &offTimeHour, &offTimeMinute);
  isTimerActive = true;
  snprintf(msg, sizeof(msg), "Timer set for all lights: ON at %02d:%02d, OFF at %02d:%02d.", onTimeHour, onTimeMinute, offTimeHour, offTimeMinute);
  publishLog(msg);
  checkTimer();
}

void ICACHE_FLASH_ATTR clearTimer() {
  isTimerActive = false;
  onTimeHour = -1;
  publishLog("Timer has been cleared for all lights.");
}

void ICACHE_FLASH_ATTR checkTimer() {
  if (!isTimerActive) return;
  int onTimeInMinutes = onTimeHour * 60 + onTimeMinute;
  int offTimeInMinutes = offTimeHour * 60 + offTimeMinute;
  int currentTimeInMinutes = timeClient.getHours() * 60 + timeClient.getMinutes();
  bool shouldBeOn = false;
  if (onTimeInMinutes < offTimeInMinutes) {
    if (currentTimeInMinutes >= onTimeInMinutes && currentTimeInMinutes < offTimeInMinutes) {
      shouldBeOn = true;
    }
  } else if (onTimeInMinutes > offTimeInMinutes) {
    if (currentTimeInMinutes >= onTimeInMinutes || currentTimeInMinutes < offTimeInMinutes) {
      shouldBeOn = true;
    }
  }
  for (int i = 0; i < NUM_LIGHTS; i++) {
    if (shouldBeOn && !lightStates[i]) {
      setLightState(i, true, "timer");
    } else if (!shouldBeOn && lightStates[i]) {
      setLightState(i, false, "timer");
    }
  }
}

// --- WEB SERVER HANDLERS ---
const char SETUP_PAGE[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Smart Home WiFi Setup</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { 
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 20px;
        }
        .container { 
            max-width: 480px; 
            width: 100%;
            background: white; 
            padding: 40px 30px; 
            border-radius: 20px; 
            box-shadow: 0 20px 60px rgba(0,0,0,0.3);
        }
        h1 { 
            color: #333;
            text-align: center; 
            margin-bottom: 30px;
            font-size: 28px;
        }
        input { 
            width: 100%; 
            padding: 14px; 
            margin: 10px 0; 
            border: 2px solid #e0e0e0; 
            border-radius: 10px; 
            font-size: 15px;
        }
        input:focus {
            outline: none;
            border-color: #667eea;
        }
        button { 
            width: 100%; 
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white; 
            padding: 16px; 
            border: none; 
            border-radius: 10px; 
            cursor: pointer; 
            font-size: 16px; 
            font-weight: 600;
            margin-top: 15px;
        }
        button:hover { 
            opacity: 0.9;
        }
        .network { 
            padding: 14px; 
            margin: 8px 0; 
            background: #f8f9fa; 
            border-radius: 10px; 
            cursor: pointer; 
            border: 2px solid transparent;
        }
        .network:hover { 
            border-color: #667eea;
            background: #f0f4ff;
        }
        .network.selected {
            border-color: #667eea;
            background: #e8edff;
        }
        .strength { 
            color: #10b981;
            font-size: 12px;
            float: right;
        }
        .loading {
            text-align: center;
            padding: 20px;
            color: #666;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>🏠 Smart Home Setup</h1>
        
        <div id="loading" class="loading">Scanning WiFi...</div>
        <div id="networks"></div>
        
        <form id="config-form" action="/save" method="POST">
            <input type="hidden" name="ssid" id="ssid" required>
            <input type="text" id="ssid-display" placeholder="Select network" readonly required>
            <input type="password" name="password" placeholder="WiFi Password" required>
            <input type="text" name="device_name" placeholder="Device Name (optional)" value="Smart Home">
            <button type="submit">💾 Save & Connect</button>
        </form>
    </div>
    
    <script>
        let selectedSSID = '';
        
        fetch('/scan')
            .then(r => r.json())
            .then(networks => {
                document.getElementById('loading').style.display = 'none';
                let html = '';
                networks.forEach(n => {
                    let strength = n.rssi > -60 ? 'Excellent' : n.rssi > -70 ? 'Good' : 'Fair';
                    html += `<div class="network" onclick="selectNetwork('${n.ssid}', this)">
                        <span>${n.ssid}</span>
                        <span class="strength">${strength}</span>
                    </div>`;
                });
                document.getElementById('networks').innerHTML = html;
            });
        
        function selectNetwork(ssid, element) {
            document.querySelectorAll('.network').forEach(n => n.classList.remove('selected'));
            element.classList.add('selected');
            selectedSSID = ssid;
            document.getElementById('ssid').value = ssid;
            document.getElementById('ssid-display').value = ssid;
        }
        
        document.getElementById('config-form').addEventListener('submit', function(e) {
            e.preventDefault();
            if (!selectedSSID) {
                alert('Please select a WiFi network');
                return;
            }
            
            fetch('/save', {
                method: 'POST',
                body: new FormData(this)
            })
            .then(() => {
                alert('✓ Configuration Saved! Device restarting...');
            });
        });
    </script>
</body>
</html>
)=====";

void handleRoot() {
  server.send_P(200, "text/html", SETUP_PAGE);
}

void handleScan() {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleSave() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  String device_name = server.arg("device_name");
  
  if (ssid.length() == 0) {
    server.send(400, "text/plain", "SSID required");
    return;
  }
  
  Serial.println("Saving config: " + ssid);
  
  writeStringToEEPROM(SSID_ADDR, ssid);
  writeStringToEEPROM(PASS_ADDR, password);
  writeStringToEEPROM(DEVICE_NAME_ADDR, device_name);
  setDeviceConfigured();
  EEPROM.commit();
  
  server.send(200, "text/plain", "OK");
  
  delay(2000);
  ESP.restart();
}
