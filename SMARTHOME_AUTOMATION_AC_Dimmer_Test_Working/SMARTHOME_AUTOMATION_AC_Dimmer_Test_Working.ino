/*
  ESP8266 Smart Home Automation System v7.0
  - AC Dimmer Control (2 Fans via TRIAC)
  - Light Control (2 Relays)
  - Environmental Sensors (Temperature, Humidity, Motion, Gas, Flame)
  - IR Remote Control
  - MQTT Communication
  
  Hardware:
  - Fan 1 TRIAC: D3 (GPIO0)
  - Fan 2 TRIAC: D4 (GPIO2)
  - Zero-Cross Detect: D7 (GPIO13)
  - Light 1 Relay: D1 (GPIO5)
  - Light 2 Relay: D2 (GPIO4)
  - Flame Sensor: D5 (GPIO14)
  - IR Receiver (VS1838B): D6 (GPIO12)
  - DHT22 (Temp/Humidity): D8 (GPIO15)
  - PIR Motion Sensor (HC-SR501): D0 (GPIO16)
  - MQ-2 Gas Sensor: A0 (Analog)

  *** CRITICAL HARDWARE WARNING ***
  GPIO0 and GPIO2 must be HIGH at boot to run the program.
  Ensure TRIAC circuits don't pull these pins LOW during boot.
*/

// --- 1. Libraries ---
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h> // For SSL (WSS)
#include <PubSubClient.h>     // For MQTT
#include <ArduinoJson.h>      // For parsing commands
#include <Ticker.h>           // For timing the TRIAC pulse
#include <NTPClient.h>        // For getting network time
#include <WiFiUdp.h>          // For NTP
#include <IRremoteESP8266.h>  // For IR remote control
#include <IRrecv.h>           // For IR receiver
#include <IRutils.h>          // For IR utilities
// #include <DHT.h>           // DHT22 now on Arduino Nano - not needed on ESP
#include <ESP8266HTTPClient.h>// For Firebase HTTP requests
#include <Wire.h>             // For I2C communication with Arduino Nano

// --- 2. WiFi & MQTT Configuration ---
const char* WIFI_SSID = "realme P1 Pro 5G";
const char* WIFI_PASS = "73213842";

// --- Arduino Nano I2C Configuration ---
#define NANO_I2C_ADDRESS 0x08  // Must match the address in Nano code
// NOTE: Hardware I2C uses D1(GPIO5)=SCL and D2(GPIO4)=SDA
// If D1/D2 are used for LEDs, you need to move the LEDs to other pins first

// Firebase Configuration
const char* FIREBASE_HOST = "smart-home-automation-2d34d-default-rtdb.firebaseio.com";
const char* FIREBASE_AUTH = "cxB1ZMQnqOBr8NwZnXDo88swBl7RnqfgScnCCtF2";

const char* MQTT_BROKER = "broker.hivemq.com";
const int   MQTT_PORT_SSL = 8883; // Port for MQTT over SSL
const char* MQTT_CLIENT_ID = "SmartHome_ESP8266_V6";

// MQTT Topics - Using main project structure
const char* fan1_set_topic = "homeautomation/project/fan/1/set";
const char* fan1_status_topic = "homeautomation/project/fan/1/status";
const char* fan2_set_topic = "homeautomation/project/fan/2/set";
const char* fan2_status_topic = "homeautomation/project/fan/2/status";
const char* light1_set_topic = "homeautomation/project/light/1/set";
const char* light1_status_topic = "homeautomation/project/light/1/status";
const char* light2_set_topic = "homeautomation/project/light/2/set";
const char* light2_status_topic = "homeautomation/project/light/2/status";
const char* flame_alarm_topic = "homeautomation/project/alarm/flame";
const char* gas_alarm_topic = "homeautomation/project/alarm/gas";
const char* motion_status_topic = "homeautomation/project/sensor/motion";
const char* temperature_status_topic = "homeautomation/project/sensor/temperature";
const char* humidity_status_topic = "homeautomation/project/sensor/humidity";
const char* sensors_status_topic = "homeautomation/project/sensor/status"; // Fixed: singular to match web
const char* esp_status_topic = "homeautomation/project/esp/status";

// PZEM-004T Power Monitor Topics
const char* pzem_voltage_topic = "homeautomation/project/pzem/voltage";
const char* pzem_current_topic = "homeautomation/project/pzem/current";
const char* pzem_power_topic = "homeautomation/project/pzem/power";
const char* pzem_energy_topic = "homeautomation/project/pzem/energy";
const char* pzem_frequency_topic = "homeautomation/project/pzem/frequency";
const char* pzem_pf_topic = "homeautomation/project/pzem/powerfactor";
const char* pzem_status_topic = "homeautomation/project/pzem/status";
const char* energy_status_topic = "homeautomation/project/energy/status";  // App expects this topic
const char* heartbeat_topic = "homeautomation/project/heartbeat";  // Heartbeat for ESP online detection

// --- 3. Hardware & Dimming Configuration ---
#define FAN1_PIN    D3  // GPIO0
#define FAN2_PIN    D4  // GPIO2
#define ZCD_PIN     D7  // GPIO13
#define LIGHT1_PIN  D1  // GPIO5 - Relay 1
#define LIGHT2_PIN  D2  // GPIO4 - Relay 2
#define FLAME_PIN   D5  // GPIO14 - Flame sensor
#define IR_RECV_PIN D6  // GPIO12 - VS1838B IR Receiver
// DHT_PIN removed - DHT22 is now on Arduino Nano D12
#define PIR_PIN     D0  // GPIO16 - HC-SR501 PIR Motion Sensor
#define MQ2_PIN     A0  // ADC - MQ-2 Gas Sensor (Analog)

// DHT22 is now on Arduino Nano - not used on ESP anymore
// #define DHTTYPE DHT22
// DHT dht(DHT_PIN, DHTTYPE);

// MQ-2 Gas Sensor Configuration
// NOTE: MQ-2 needs 24-48 hours burn-in for accurate readings
// Initial readings may be high during calibration period
#define GAS_THRESHOLD 700  // Analog value threshold for gas detection (0-1024) - Increased for burn-in
#define GAS_SAFE_LEVEL 500 // Below this is considered safe - Increased for burn-in

#define AC_FREQ 50 // 50 Hz.
#define HALF_CYCLE_MICROS (1000000 / (AC_FREQ * 2)) // 10000us for 50Hz
#define TRIAC_PULSE_MICROS 20 // How long to pulse the TRIAC gate (in microseconds)
#define DIMMER_MIN_DELAY 200  // Minimum delay after ZCD to fire TRIAC (us)

// IR Remote Control Codes
#define IR_ALL_DEVICES    0x20DF10EF  // All devices ON/OFF toggle
#define IR_LIGHT1         0x20DF8877  // Light 1 ON/OFF toggle
#define IR_LIGHT2         0x20DF48B7  // Light 2 ON/OFF toggle
#define IR_FAN1           0x20DFC837  // Fan 1 ON/OFF toggle
#define IR_FAN2           0x20DF28D7  // Fan 2 ON/OFF toggle
#define IR_SPEED_UP       0x20DF40BF  // Increase fan speed (+10%)
#define IR_SPEED_DOWN     0x20DFC03F  // Decrease fan speed (-10%)

#define FAN_SPEED_STEP    10  // Fan speed increment/decrement step

// --- 4. Global Variables ---
WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);
Ticker fan1Timer;
Ticker fan2Timer;

// Network Time - IST timezone (GMT+5:30 = 19800 seconds)
WiFiUDP ntpUDP;
const long GMT_OFFSET_SEC = 19800;  // IST offset for display
const int UPDATE_INTERVAL_MS = 60000;  // Update every 60 seconds
NTPClient timeClient(ntpUDP, "pool.ntp.org", GMT_OFFSET_SEC, UPDATE_INTERVAL_MS);

// Helper function to get UTC epoch time (without timezone offset)
unsigned long getUTCEpochTime() {
  return timeClient.getEpochTime() - GMT_OFFSET_SEC;
}

// IR Receiver
IRrecv irrecv(IR_RECV_PIN);
decode_results results;

// Sensor readings
float temperature = 0.0;
float humidity = 0.0;
int gasLevel = 0;
bool motionDetected = false;
bool gasAlarmActive = false;
bool flameDetected = false;
bool lastFlameState = false;
unsigned long lastFlameCheck = 0;
const unsigned long FLAME_CHECK_INTERVAL = 500; // Check flame sensor every 500ms

// Arduino Nano I2C Sensor Data
bool nanoMotionDetected = false;
float nanoTemperature = 0.0;
int nanoGasLevel = 0;
byte nanoFanSpeed = 0;
bool nanoLight1State = false;
bool nanoLight2State = false;

// PZEM-004T data from Nano
float pzemVoltage = 0.0;
float pzemCurrent = 0.0;
float pzemPower = 0.0;
float pzemEnergy = 0.0;
float pzemFrequency = 0.0;
float pzemPowerFactor = 0.0;

unsigned long lastNanoRead = 0;
const unsigned long NANO_READ_INTERVAL = 3000; // Read from Nano every 3 seconds

// PIR Motion Detection with timeout (for H mode sensors without jumper)
unsigned long lastMotionTime = 0;
const unsigned long MOTION_TIMEOUT = 10000; // 10 seconds - motion clears after this time
unsigned long lastMotionCheckTime = 0;
const unsigned long MOTION_RECHECK_INTERVAL = 2000; // Check every 2 seconds

// Timing variables
unsigned long lastSensorRead = 0;
const unsigned long SENSOR_READ_INTERVAL = 5000; // Read sensors every 5 seconds
unsigned long lastMotionCheck = 0;
const unsigned long MOTION_CHECK_INTERVAL = 1000; // Check motion every 1 second
unsigned long lastFirebaseUpdate = 0;
const unsigned long FIREBASE_UPDATE_INTERVAL = 60000; // Update Firebase every 60 seconds (reduced to prevent stack overflow)
unsigned long lastHistorySave = 0;
const unsigned long HISTORY_SAVE_INTERVAL = 300000; // Save to history every 5 minutes (reduced for stability)
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 10000; // Send heartbeat every 10 seconds (optimized for faster detection)

// Volatile variables are used because they are modified by an Interrupt
// and read in the main loop / MQTT callback.
volatile int fan1Power = 0; // Power level 0-100
volatile int fan2Power = 0; // Power level 0-100
volatile unsigned long zcdCounter = 0; // Debug: count ZCD interrupts

// Pre-calculated delay values for dimming (to avoid float calculations in ISR)
volatile float fan1Delay = 0.0;
volatile float fan2Delay = 0.0;

// Light states
bool light1State = false;
bool light2State = false;

// Flame sensor
bool isFlameDetected = false;
bool flameProtectionEnabled = false; // Disabled by default

// IR Remote control state tracking
bool allDevicesState = false; // Track overall state for all devices toggle

// IR command processing
uint32_t pendingIRCommand = 0; // Store IR command to process
bool hasIRCommand = false;      // Flag to indicate new command
unsigned long lastIRProcess = 0; // Last time IR command was processed
const unsigned long IR_PROCESS_INTERVAL = 200; // Process IR every 200ms (prevents flooding)
unsigned long lastIRCommand = 0;
#define IR_DEBOUNCE_DELAY 250 // Milliseconds to debounce IR commands

// --- 5. TRIAC Trigger Functions (Called by Ticker) ---
// These functions MUST be in IRAM to be called reliably by the ISR/Ticker
void ICACHE_RAM_ATTR triggerFan1() {
  digitalWrite(FAN1_PIN, HIGH);
  delayMicroseconds(TRIAC_PULSE_MICROS);
  digitalWrite(FAN1_PIN, LOW);
}

void ICACHE_RAM_ATTR triggerFan2() {
  digitalWrite(FAN2_PIN, HIGH);
  delayMicroseconds(TRIAC_PULSE_MICROS);
  digitalWrite(FAN2_PIN, LOW);
}

// --- 6. Zero-Cross Detection (ZCD) Interrupt Service Routine ---
// This is the heart of the dimmer. It fires on every zero-cross.
// OPTIMIZED: Uses pre-calculated delays to minimize ISR execution time
void ICACHE_RAM_ATTR zeroCross() {
  zcdCounter++; // Debug counter
  
  // Use pre-calculated delays (calculated in main loop, not in ISR)
  // This keeps ISR execution time minimal to prevent watchdog issues
  
  if (fan1Power > 5 && fan1Delay > 0.0) {
    fan1Timer.once(fan1Delay, triggerFan1);
  }

  if (fan2Power > 5 && fan2Delay > 0.0) {
    fan2Timer.once(fan2Delay, triggerFan2);
  }
}

// --- 6.5 Calculate Dimming Delays (Called from main loop, NOT interrupt) ---
void updateDimmingDelays() {
  // Pre-calculate delays for ISR to use
  // This prevents float math in interrupt context
  if (fan1Power > 5) {
    float delayPercent = (100.0 - fan1Power) / 100.0;
    fan1Delay = (delayPercent * (HALF_CYCLE_MICROS - DIMMER_MIN_DELAY) / 1000000.0) + (DIMMER_MIN_DELAY / 1000000.0);
  } else {
    fan1Delay = 0.0;
  }
  
  if (fan2Power > 5) {
    float delayPercent = (100.0 - fan2Power) / 100.0;
    fan2Delay = (delayPercent * (HALF_CYCLE_MICROS - DIMMER_MIN_DELAY) / 1000000.0) + (DIMMER_MIN_DELAY / 1000000.0);
  } else {
    fan2Delay = 0.0;
  }
}

// --- 7. MQTT & WiFi Functions ---
void setupWiFi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // --- Sync Time using NTP ---
  Serial.println("Fetching NTP time...");
  timeClient.begin();
  delay(1000); // Wait for UDP to initialize
  
  int retries = 0;
  while (!timeClient.update() && retries < 3) {
    Serial.print("⏳ Retry ");
    Serial.println(retries + 1);
    delay(2000);
    retries++;
  }
  
  if (retries < 3) {
    Serial.print("✅ Time synced (IST): ");
    Serial.println(timeClient.getFormattedTime());
    Serial.print("📅 Epoch time: ");
    Serial.println(timeClient.getEpochTime());
  } else {
    Serial.println("⚠️ Failed to fetch NTP time after 3 attempts.");
  }
  // Keep timeClient running, don't call timeClient.end()
}

// This function is called every time a message arrives
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  yield(); // Feed watchdog immediately at callback start
  String topicStr = String(topic);
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);
  
  // Handle status requests from app (for refresh button)
  if (topicStr.indexOf("/get") > 0 && message == "status") {
    Serial.println("📊 Status request received");
    
    // Respond with current device states
    if (topicStr.indexOf("light/1/get") > 0) {
      mqttClient.publish(light1_status_topic, nanoLight1State ? "ON" : "OFF", false);
    }
    else if (topicStr.indexOf("light/2/get") > 0) {
      mqttClient.publish(light2_status_topic, nanoLight2State ? "ON" : "OFF", false);
    }
    else if (topicStr.indexOf("fan/1/get") > 0) {
      mqttClient.publish(fan1_status_topic, String(fan1Power).c_str(), false);
    }
    else if (topicStr.indexOf("fan/2/get") > 0) {
      mqttClient.publish(fan2_status_topic, String(fan2Power).c_str(), false);
    }
    else if (topicStr.indexOf("energy/get") > 0) {
      // Immediately read and publish PZEM data
      readNanoSensorData();
    }
    return; // Don't process as regular command
  }
  
  // Handle Fan 1
  if (topicStr == fan1_set_topic) {
    int value = message.toInt();
    value = constrain(value, 0, 100);
    fan1Power = value;
    updateDimmingDelays(); // Recalculate delay immediately
    Serial.print("✓ Fan 1 set to ");
    Serial.print(fan1Power);
    Serial.println("%");
    yield(); // Feed watchdog
    
    // Use char buffer instead of String to avoid heap fragmentation
    char speedBuf[8];
    snprintf(speedBuf, sizeof(speedBuf), "%d", fan1Power);
    mqttClient.publish(fan1_status_topic, speedBuf, false);
    yield(); // Feed watchdog after publish
  }
  // Handle Fan 2
  else if (topicStr == fan2_set_topic) {
    int value = message.toInt();
    value = constrain(value, 0, 100);
    fan2Power = value;
    updateDimmingDelays(); // Recalculate delay immediately
    Serial.print("✓ Fan 2 set to ");
    Serial.print(fan2Power);
    Serial.println("%");
    yield(); // Feed watchdog
    
    // Use char buffer instead of String to avoid heap fragmentation
    char speedBuf[8];
    snprintf(speedBuf, sizeof(speedBuf), "%d", fan2Power);
    mqttClient.publish(fan2_status_topic, speedBuf, false);
    yield(); // Feed watchdog after publish
  }
  // Handle Light 1
  else if (topicStr == light1_set_topic) {
    light1State = (message == "ON" || message == "1");
    // Send command to Arduino Nano via I2C
    sendCommandToNano(light1State ? 0x01 : 0x02, 0);
    Serial.print("✓ Light 1 (via Nano): ");
    Serial.println(light1State ? "ON" : "OFF");
    mqttClient.publish(light1_status_topic, light1State ? "ON" : "OFF", false); // Changed to non-retained for faster response
  }
  // Handle Light 2
  else if (topicStr == light2_set_topic) {
    light2State = (message == "ON" || message == "1");
    // Send command to Arduino Nano via I2C
    sendCommandToNano(light2State ? 0x03 : 0x04, 0);
    Serial.print("✓ Light 2 (via Nano): ");
    Serial.println(light2State ? "ON" : "OFF");
    mqttClient.publish(light2_status_topic, light2State ? "ON" : "OFF", false); // Changed to non-retained for faster response
  }
}

void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Attempting MQTT connection...");
    
    // --- FIX for rc=-2: Call setInsecure() ---
    // This skips certificate validation.
    // Necessary because ESP8266 may not have HiveMQ's Root CA.
    wifiClient.setInsecure();
  
    if (mqttClient.connect(MQTT_CLIENT_ID, NULL, NULL, esp_status_topic, 1, true, "offline")) {
      Serial.println("connected");
      
      // Subscribe to all control topics
      mqttClient.subscribe(fan1_set_topic);
      mqttClient.subscribe(fan2_set_topic);
      mqttClient.subscribe(light1_set_topic);
      mqttClient.subscribe(light2_set_topic);
      
      // Subscribe to status request topics (for app refresh button)
      mqttClient.subscribe("homeautomation/project/light/1/get");
      mqttClient.subscribe("homeautomation/project/light/2/get");
      mqttClient.subscribe("homeautomation/project/fan/1/get");
      mqttClient.subscribe("homeautomation/project/fan/2/get");
      mqttClient.subscribe("homeautomation/project/energy/get");
      
      Serial.println("✓ Subscribed to all topics (including get topics)");
      
      // Publish online status
      mqttClient.publish(esp_status_topic, "online", true);
      
      // Publish current states (non-retained for faster response)
      char speed1Buf[8], speed2Buf[8];
      snprintf(speed1Buf, sizeof(speed1Buf), "%d", fan1Power);
      snprintf(speed2Buf, sizeof(speed2Buf), "%d", fan2Power);
      mqttClient.publish(fan1_status_topic, speed1Buf, false);
      mqttClient.publish(fan2_status_topic, speed2Buf, false);
      mqttClient.publish(light1_status_topic, light1State ? "ON" : "OFF", false);
      mqttClient.publish(light2_status_topic, light2State ? "ON" : "OFF", false);
      
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 2 seconds");
      delay(2000); // Reduced from 5s to 2s for faster recovery
    }
  }
}

// --- 7.5 IR Remote Control Handler ---
void handleIRCommand(uint32_t irCode) {
  // Debounce - ignore commands received too quickly
  if (millis() - lastIRCommand < IR_DEBOUNCE_DELAY) {
    return;
  }
  lastIRCommand = millis();

  Serial.print("🔵 IR: 0x");
  Serial.println(irCode, HEX);

  // Process commands quickly to prevent watchdog reset
  yield(); // Feed the watchdog

  switch (irCode) {
    case IR_ALL_DEVICES:
      // Toggle all devices
      allDevicesState = !allDevicesState;
      
      // Set all lights via Arduino Nano
      light1State = allDevicesState;
      light2State = allDevicesState;
      sendCommandToNano(light1State ? 0x01 : 0x02, 0);
      yield(); // Feed watchdog after I2C
      sendCommandToNano(light2State ? 0x03 : 0x04, 0);
      yield(); // Feed watchdog after I2C
      
      // Set all fans
      fan1Power = allDevicesState ? 75 : 0;
      fan2Power = allDevicesState ? 75 : 0;
      updateDimmingDelays(); // Recalculate delays
      
      // Publish MQTT status (non-blocking)
      if (mqttClient.connected()) {
        mqttClient.publish(light1_status_topic, light1State ? "ON" : "OFF", false);
        yield();
        mqttClient.publish(light2_status_topic, light2State ? "ON" : "OFF", false);
        yield();
        char speed1Buf[8], speed2Buf[8];
        snprintf(speed1Buf, sizeof(speed1Buf), "%d", fan1Power);
        snprintf(speed2Buf, sizeof(speed2Buf), "%d", fan2Power);
        mqttClient.publish(fan1_status_topic, speed1Buf, false);
        yield();
        mqttClient.publish(fan2_status_topic, speed2Buf, false);
      }
      
      Serial.println(allDevicesState ? "✓ ALL ON" : "✓ ALL OFF");
      break;

    case IR_LIGHT1:
      light1State = !light1State;
      sendCommandToNano(light1State ? 0x01 : 0x02, 0);
      yield();
      if (mqttClient.connected()) {
        mqttClient.publish(light1_status_topic, light1State ? "ON" : "OFF", true);
      }
      Serial.print("✓ L1: ");
      Serial.println(light1State ? "ON" : "OFF");
      break;

    case IR_LIGHT2:
      light2State = !light2State;
      sendCommandToNano(light2State ? 0x03 : 0x04, 0);
      yield();
      if (mqttClient.connected()) {
        mqttClient.publish(light2_status_topic, light2State ? "ON" : "OFF", true);
      }
      Serial.print("✓ L2: ");
      Serial.println(light2State ? "ON" : "OFF");
      break;

    case IR_FAN1:
      fan1Power = (fan1Power > 0) ? 0 : 75;
      updateDimmingDelays(); // Recalculate delays
      if (mqttClient.connected()) {
        char speedBuf[8];
        snprintf(speedBuf, sizeof(speedBuf), "%d", fan1Power);
        mqttClient.publish(fan1_status_topic, speedBuf, false);
      }
      Serial.print("✓ F1: ");
      Serial.print(fan1Power);
      Serial.println("%");
      break;

    case IR_FAN2:
      fan2Power = (fan2Power > 0) ? 0 : 75;
      updateDimmingDelays(); // Recalculate delays
      if (mqttClient.connected()) {
        mqttClient.publish(fan2_status_topic, String(fan2Power).c_str(), true);
      }
      Serial.print("✓ F2: ");
      Serial.print(fan2Power);
      Serial.println("%");
      break;

    case IR_SPEED_UP:
      // Increase speed of active fans only
      if (fan1Power > 0 && fan1Power < 100) {
        fan1Power = constrain(fan1Power + FAN_SPEED_STEP, 0, 100);
      } else if (fan1Power == 0 && fan2Power == 0) {
        fan1Power = fan2Power = FAN_SPEED_STEP;
      }
      
      if (fan2Power > 0 && fan2Power < 100) {
        fan2Power = constrain(fan2Power + FAN_SPEED_STEP, 0, 100);
      }
      
      updateDimmingDelays(); // Recalculate delays
      yield();
      
      // Publish both fan states
      if (mqttClient.connected()) {
        char speed1Buf[8], speed2Buf[8];
        snprintf(speed1Buf, sizeof(speed1Buf), "%d", fan1Power);
        snprintf(speed2Buf, sizeof(speed2Buf), "%d", fan2Power);
        mqttClient.publish(fan1_status_topic, speed1Buf, false);
        yield();
        mqttClient.publish(fan2_status_topic, speed2Buf, false);
      }
      Serial.println("✓ Speed UP");
      break;

    case IR_SPEED_DOWN:
      // Decrease speed of active fans only
      if (fan1Power > 0) {
        fan1Power = constrain(fan1Power - FAN_SPEED_STEP, 0, 100);
      }
      
      if (fan2Power > 0) {
        fan2Power = constrain(fan2Power - FAN_SPEED_STEP, 0, 100);
      }
      
      updateDimmingDelays(); // Recalculate delays
      yield();
      
      // Publish both fan states
      if (mqttClient.connected()) {
        char speed1Buf[8], speed2Buf[8];
        snprintf(speed1Buf, sizeof(speed1Buf), "%d", fan1Power);
        snprintf(speed2Buf, sizeof(speed2Buf), "%d", fan2Power);
        mqttClient.publish(fan1_status_topic, speed1Buf, false);
        yield();
        mqttClient.publish(fan2_status_topic, speed2Buf, false);
      }
      Serial.println("✓ Speed DOWN");
      break;

    default:
      // Unknown IR code - ignore silently
      break;
  }
  
  yield(); // Final yield after processing command
}

// --- 7.6 Firebase Functions ---
// DISABLED: Firebase operations cause stack overflow when combined with interrupts
// Use MQTT for all real-time data - web interface reads from Firebase Realtime DB
void sendToFirebase(String path, String jsonData) {
  // Firebase DISABLED to prevent stack smashing
  // All data is sent via MQTT and web interface handles Firebase writes
  // This prevents stack overflow from HTTPClient + SSL + interrupts
  yield();
  // Serial.println("✅ Firebase skipped (MQTT only)");
}

// This function is no longer needed - sensor data is published directly in readAndPublishSensorData()
// Kept for compatibility but does nothing (MQTT publishing moved to readAndPublishSensorData)
void saveCurrentSensorData() {
  // Firebase disabled - MQTT publishing handled in readAndPublishSensorData()
  yield();
}

// Save PZEM power monitoring data to Firebase
void savePZEMData() {
  // Only save if we have valid voltage data
  if (pzemVoltage < 10.0) {
    return; // No valid data
  }
  
  // ALWAYS publish to MQTT for real-time monitoring (even with no load)
  if (mqttClient.connected()) {
    StaticJsonDocument<256> mqttDoc;
    mqttDoc["voltage"] = pzemVoltage;
    mqttDoc["current"] = pzemCurrent;
    mqttDoc["power"] = pzemPower;
    mqttDoc["energy"] = pzemEnergy;
    mqttDoc["frequency"] = pzemFrequency;
    mqttDoc["pf"] = pzemPowerFactor;
    
    char mqttBuffer[256];
    serializeJson(mqttDoc, mqttBuffer);
    
    // Publish to BOTH topics for compatibility
    mqttClient.publish(pzem_status_topic, mqttBuffer, false);
    mqttClient.publish(energy_status_topic, mqttBuffer, false);  // App expects this topic
    
    Serial.println("✅ PZEM data published to MQTT");
  }
  
  // Only save to Firebase if current is flowing (load is connected)
  // This prevents displaying old/stale data when no load is active
  if (pzemCurrent < 0.01) {
    // No load connected - don't update Firebase current data
    // This way the website will show old timestamp and detect "No Load" state
    return;
  }
  
  timeClient.update();
  
  // Save CURRENT data (all metrics for real-time display)
  StaticJsonDocument<512> doc;
  doc["voltage"] = pzemVoltage;
  doc["current"] = pzemCurrent;
  doc["power"] = pzemPower;
  doc["energy"] = pzemEnergy;
  doc["frequency"] = pzemFrequency;
  doc["powerFactor"] = pzemPowerFactor;
  
  unsigned long epochTime = getUTCEpochTime();
  unsigned long long timestampMs = (unsigned long long)epochTime * 1000ULL;
  doc["timestamp"] = timestampMs;
  doc["lastUpdate"] = timeClient.getFormattedTime();
  
  char buffer[512];
  serializeJson(doc, buffer);
  
  // Firebase disabled to prevent stack overflow - MQTT provides all real-time data
  // sendToFirebase("/powerMonitor/current", buffer);
  yield();
  
  // Save HISTORY data (for charts and analytics)
  StaticJsonDocument<512> historyDoc;
  historyDoc["voltage"] = pzemVoltage;
  historyDoc["current"] = pzemCurrent;
  historyDoc["power"] = pzemPower;
  historyDoc["energy"] = pzemEnergy;  // Store energy for cost calculation
  historyDoc["timestamp"] = timestampMs;
  
  char historyBuffer[512];
  serializeJson(historyDoc, historyBuffer);
  
  // Generate unique key using timestamp
  String historyPath = "/powerMonitor/history/" + String(timestampMs);
  // Firebase disabled to prevent stack overflow
  // sendToFirebase(historyPath.c_str(), historyBuffer);
  yield();
  
  Serial.println("💾 Saved PZEM data (current + history)");
}

void saveHistoricalData() {
  StaticJsonDocument<512> doc;
  doc["temperature"] = temperature;
  doc["humidity"] = humidity;
  doc["gasLevel"] = gasLevel;
  
  unsigned long epochTime = getUTCEpochTime();  // Use UTC time
  unsigned long long timestampMs = (unsigned long long)epochTime * 1000ULL;
  doc["timestamp"] = timestampMs;
  
  Serial.print("🕐 UTC Epoch (s): ");
  Serial.print(epochTime);
  Serial.print(" → Timestamp (ms): ");
  Serial.println(timestampMs);
  
  char buffer[512];
  serializeJson(doc, buffer);
  Serial.print("📤 Sending: ");
  Serial.println(buffer);
  
  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(512, 512);
  
  HTTPClient http;
  String url = "https://" + String(FIREBASE_HOST) + "/sensors/history.json?auth=" + String(FIREBASE_AUTH);
  
  if (!http.begin(client, url)) {
    return;
  }
  
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);
  
  int httpCode = http.POST(buffer);
  
  if (httpCode == HTTP_CODE_OK || httpCode == 200) {
    Serial.println("✅ History saved");
  }
  
  http.end();
  yield();
}

void updateFirebaseAlarms() {
  if (gasAlarmActive) {
    StaticJsonDocument<256> doc;
    doc["active"] = true;
    doc["level"] = gasLevel;
    doc["message"] = "High gas concentration detected. Please ventilate the area immediately.";
    doc["timestamp"] = timeClient.getEpochTime() * 1000;
    
    char buffer[256];
    serializeJson(doc, buffer);
    
    sendToFirebase("/alarms/gas", buffer);
    Serial.println("🚨 Gas alarm updated in Firebase");
  }
}

// --- 7.7 Sensor Reading Functions ---
void readAndPublishSensorData() {
  // DHT22 is now on Arduino Nano - skip reading here
  // Temperature and humidity are received via I2C from Nano
  
  // Read MQ-2 Gas Sensor (Analog)
  gasLevel = analogRead(MQ2_PIN);
  
  // Only print and publish if we have valid data
  if (temperature > 0 || humidity > 0) {
    Serial.println("--- Environmental Sensors ---");
    Serial.print("🌡️  Temperature: "); Serial.print(temperature); Serial.println(" °C");
    Serial.print("💧 Humidity: "); Serial.print(humidity); Serial.println(" %");
    Serial.print("💨 Gas Level: "); Serial.print(gasLevel); Serial.print("/1024");
    
    // Check gas level
    if (gasLevel > GAS_THRESHOLD) {
      Serial.println(" ⚠️ HIGH (DANGER!)");
      if (!gasAlarmActive) {
        gasAlarmActive = true;
        mqttClient.publish(gas_alarm_topic, "DETECTED", true);
        Serial.println("🚨 GAS ALARM TRIGGERED!");
      }
    } else if (gasLevel > GAS_SAFE_LEVEL) {
      Serial.println(" ⚠️ MODERATE");
      gasAlarmActive = false;
    } else {
      Serial.println(" ✅ SAFE");
      if (gasAlarmActive) {
        gasAlarmActive = false;
        mqttClient.publish(gas_alarm_topic, "CLEAR", true);
        Serial.println("✓ Gas alarm cleared");
      }
    }
    Serial.println("---------------------------");
  }

  // ALWAYS publish to MQTT for real-time monitoring (every 5 seconds)
  timeClient.update();
  
  StaticJsonDocument<512> mqttDoc;
  mqttDoc["temperature"] = temperature;
  mqttDoc["humidity"] = humidity;
  mqttDoc["gasLevel"] = gasLevel;
  mqttDoc["gasAlarm"] = gasAlarmActive;
  mqttDoc["motion"] = motionDetected;
  
  unsigned long epochTime = getUTCEpochTime();
  unsigned long long timestampMs = (unsigned long long)epochTime * 1000ULL;
  mqttDoc["timestamp"] = timestampMs;
  mqttDoc["lastUpdate"] = timeClient.getFormattedTime();

  char mqttBuffer[512];
  serializeJson(mqttDoc, mqttBuffer);

  // Publish to MQTT for real-time web interface updates
  if (mqttClient.connected()) {
    mqttClient.publish(sensors_status_topic, mqttBuffer, false);  // Non-retained for sensor data
    Serial.println("✅ Sensor data published to MQTT");
  }
  
  // Firebase disabled to prevent stack overflow - saveCurrentSensorData() no longer needed
  // Web interface handles Firebase persistence from MQTT data
  yield();
}

// --- Arduino Nano I2C Communication Functions ---

// Send command to Arduino Nano
void sendCommandToNano(byte command, byte data) {
  Wire.beginTransmission(NANO_I2C_ADDRESS);
  Wire.write(command);
  Wire.write(data);
  byte error = Wire.endTransmission();
  
  yield(); // Feed watchdog after I2C
  
  if (error != 0) {
    Serial.print("❌ I2C Err: ");
    Serial.println(error);
  }
  // Reduced logging - only show errors to prevent serial flooding
}

// Read sensor data from Arduino Nano
void readNanoSensorData() {
  const byte DATA_SIZE = 32;  // Updated to 32 bytes to include PZEM data
  byte receivedData[DATA_SIZE];
  
  // Request 32 bytes from Nano
  Wire.requestFrom(NANO_I2C_ADDRESS, (uint8_t)DATA_SIZE);
  
  if (Wire.available() >= DATA_SIZE) {
    for (int i = 0; i < DATA_SIZE; i++) {
      receivedData[i] = Wire.read();
    }
    
    // Parse basic sensor data (bytes 0-5)
    nanoMotionDetected = receivedData[0];
    nanoTemperature = ((receivedData[1] << 8) | receivedData[2]) / 10.0;  // DHT22 temp from Nano
    float nanoHumidity = ((receivedData[3] << 8) | receivedData[4]) / 10.0;  // DHT22 humidity from Nano
    nanoFanSpeed = receivedData[5];
    
    // Use Nano's DHT22 data for ESP's temperature and humidity
    temperature = nanoTemperature;
    humidity = nanoHumidity;
    
    // Parse PZEM data (bytes 6-17) - values are multiplied by 100
    int voltageInt = (receivedData[6] << 8) | receivedData[7];
    int currentInt = (receivedData[8] << 8) | receivedData[9];
    int powerInt = (receivedData[10] << 8) | receivedData[11];
    int energyInt = (receivedData[12] << 8) | receivedData[13];
    int frequencyInt = (receivedData[14] << 8) | receivedData[15];
    int pfInt = (receivedData[16] << 8) | receivedData[17];
    
    pzemVoltage = voltageInt / 100.0;
    pzemCurrent = currentInt / 100.0;
    pzemPower = powerInt;
    pzemEnergy = energyInt / 100.0;
    pzemFrequency = frequencyInt / 100.0;
    pzemPowerFactor = pfInt / 100.0;
    
    // Parse light states (bytes 18-19)
    nanoLight1State = receivedData[18];
    nanoLight2State = receivedData[19];
    
    Serial.println("📥 Received data from Nano:");
    Serial.print("  - Motion: ");
    Serial.println(nanoMotionDetected ? "DETECTED" : "CLEAR");
    Serial.print("  - Temperature: ");
    Serial.print(nanoTemperature);
    Serial.println("°C");
    Serial.print("  - Gas Level: ");
    Serial.println(nanoGasLevel);
    Serial.print("  - Fan Speed: ");
    Serial.println(nanoFanSpeed);
    
    // Print PZEM data if valid
    if (pzemVoltage > 0) {
      Serial.println("  ⚡ PZEM-004T Data:");
      Serial.print("    Voltage: "); Serial.print(pzemVoltage); Serial.println(" V");
      Serial.print("    Current: "); Serial.print(pzemCurrent); Serial.println(" A");
      Serial.print("    Power: "); Serial.print(pzemPower); Serial.println(" W");
      Serial.print("    Energy: "); Serial.print(pzemEnergy); Serial.println(" kWh");
      Serial.print("    Frequency: "); Serial.print(pzemFrequency); Serial.println(" Hz");
      Serial.print("    PF: "); Serial.println(pzemPowerFactor);
      
      // Save PZEM data to Firebase and publish to MQTT
      savePZEMData();
    }
  } else {
    Serial.println("⚠️ Nano did not send enough data");
  }
}

// Check PIR Motion Sensor
void checkMotionSensor() {
  unsigned long currentTime = millis();
  
  // Only check every MOTION_RECHECK_INTERVAL to avoid flooding
  if (currentTime - lastMotionCheckTime < MOTION_RECHECK_INTERVAL) {
    return;
  }
  lastMotionCheckTime = currentTime;
  
  bool currentPirState = digitalRead(PIR_PIN);
  
  if (currentPirState == HIGH) {
    // PIR is HIGH - motion detected
    
    if (!motionDetected) {
      // Motion just started
      motionDetected = true;
      lastMotionTime = currentTime;
      Serial.println("🚶 MOTION DETECTED!");
      mqttClient.publish(motion_status_topic, "DETECTED", true);
    } else {
      // Motion still active - update timestamp
      lastMotionTime = currentTime;
      Serial.println("👁️ Motion still active...");
    }
  } else {
    // PIR is LOW - no motion
    
    if (motionDetected) {
      // Check if enough time has passed since last motion
      if (currentTime - lastMotionTime >= MOTION_TIMEOUT) {
        motionDetected = false;
        Serial.println("✓ Motion cleared (no detection for 10 seconds)");
        mqttClient.publish(motion_status_topic, "CLEAR", true);
      }
    }
  }
  
  // Force clear if motion has been active too long (sensor stuck)
  if (motionDetected && (currentTime - lastMotionTime > 60000)) {
    motionDetected = false;
    Serial.println("⚠️ Force clearing motion (stuck for 60+ seconds)");
    mqttClient.publish(motion_status_topic, "CLEAR", true);
  }
}

// Check flame sensor and publish alarm
void checkFlameSensor() {
  // Flame sensor: LOW = Fire detected, HIGH = No fire
  flameDetected = (digitalRead(FLAME_PIN) == LOW);
  
  // Only publish on state change to avoid flooding
  if (flameDetected && !lastFlameState) {
    // Fire just detected!
    Serial.println("🔥🔥🔥 FIRE DETECTED! 🔥🔥🔥");
    Serial.println("⚠️  Publishing fire alarm to MQTT...");
    mqttClient.publish(flame_alarm_topic, "CLEAR", true); // "CLEAR" means fire detected
    lastFlameState = true;
  } 
  else if (!flameDetected && lastFlameState) {
    // Fire cleared
    Serial.println("✓ Fire alarm cleared - no flame detected");
    mqttClient.publish(flame_alarm_topic, "DETECTED", true); // "DETECTED" means no fire
    lastFlameState = false;
  }
}

// --- 8. Setup & Loop ---
void setup() {
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println("  Smart Home Automation System v7.0");
  Serial.println("  Full Sensor Integration");
  Serial.println("========================================");
  
  // Setup Pin Modes - Fans
  pinMode(FAN1_PIN, OUTPUT);
  pinMode(FAN2_PIN, OUTPUT);
  digitalWrite(FAN1_PIN, LOW);
  digitalWrite(FAN2_PIN, LOW);

  // Lights are now controlled by Arduino Nano via I2C
  // No pinMode setup needed for LIGHT1_PIN and LIGHT2_PIN

  // Setup Pin Modes - Sensors
  pinMode(FLAME_PIN, INPUT_PULLUP);
  pinMode(PIR_PIN, INPUT);
  pinMode(MQ2_PIN, INPUT);  // Analog pin
  
  // DHT22 is now on Arduino Nano - skip initialization
  Serial.println("✓ DHT22 is on Arduino Nano D12 (will receive via I2C)");

  // Setup IR Receiver
  irrecv.enableIRIn();
  Serial.println("✓ IR Receiver initialized on D6 (GPIO12)");
  
  // Setup PIR Motion Sensor
  pinMode(PIR_PIN, INPUT);
  Serial.println("✓ PIR Motion sensor initialized on D0 (GPIO16)");
  Serial.println("⏳ Calibrating PIR sensor (please wait 60 seconds)...");
  
  // PIR sensors need 30-60 seconds to calibrate after power-on
  // During this time, ignore all readings
  unsigned long pirStartTime = millis();
  while (millis() - pirStartTime < 60000) {
    if ((millis() - pirStartTime) % 10000 == 0) {
      Serial.print(".");
    }
    delay(1000);
  }
  Serial.println("\n✅ PIR sensor calibrated and ready!");
  
  // Read initial state
  motionDetected = digitalRead(PIR_PIN);
  Serial.print("Initial motion state: ");
  Serial.println(motionDetected ? "DETECTED" : "CLEAR");
  
  // Setup MQ-2 Gas Sensor
  Serial.println("✓ MQ-2 Gas sensor initialized on A0");

  // ZCD pin - Using FALLING edge (tested and working)
  pinMode(ZCD_PIN, INPUT_PULLUP);

  // Setup WiFi & Sync Time
  setupWiFi();
  
  // Initialize I2C as Master (Hardware I2C: D1=SCL, D2=SDA)
  Wire.begin();
  Serial.println("✓ I2C Master initialized (SDA=D2/GPIO4, SCL=D1/GPIO5)");
  delay(500);
  
  // Test if Nano is connected
  Wire.beginTransmission(NANO_I2C_ADDRESS);
  byte error = Wire.endTransmission();
  if (error == 0) {
    Serial.println("✓ Arduino Nano detected on I2C at address 0x08");
  } else {
    Serial.println("⚠️ Arduino Nano NOT detected - Check connections!");
  }
  
  // Keep time client running for timestamps
  timeClient.begin();
  timeClient.setUpdateInterval(60000);  // Update every minute;

  // --- MQTT SSL Setup ---
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT_SSL);
  mqttClient.setCallback(mqttCallback);

  // Attach the interrupt to the ZCD pin
  attachInterrupt(digitalPinToInterrupt(ZCD_PIN), zeroCross, FALLING);
  
  Serial.println("✓ Zero-cross interrupt attached (FALLING edge)");
  Serial.println("✓ All systems ready!");
  Serial.println("========================================\n");
  
  // Initial sensor reading
  delay(2000);  // DHT22 needs time after power-on
  readAndPublishSensorData();
}

void loop() {
  static unsigned long lastZcdReport = 0;
  static unsigned long lastZcdCount = 0;
  
  // Critical: yield frequently to prevent watchdog reset
  yield();
  
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop(); // This handles MQTT keep-alives and message processing
  yield(); // Feed watchdog after MQTT processing
  
  // Handle IR Remote Control - Just capture the command
  if (irrecv.decode(&results)) {
    if (results.value != 0xFFFFFFFF && results.value != 0) {  // Ignore repeat codes and invalid
      // Store command for later processing
      pendingIRCommand = results.value;
      hasIRCommand = true;
    }
    irrecv.resume();  // Ready for next signal
  }
  
  // Process IR command (rate-limited to prevent flooding)
  if (hasIRCommand && (millis() - lastIRProcess >= IR_PROCESS_INTERVAL)) {
    handleIRCommand(pendingIRCommand);
    hasIRCommand = false;
    lastIRProcess = millis();
  }
  
  yield(); // Feed watchdog after IR processing
  
  // Read environmental sensors periodically
  if (millis() - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = millis();
    readAndPublishSensorData();
    yield(); // Feed watchdog after sensor reading
  }
  
  // Read Arduino Nano sensor data periodically
  if (millis() - lastNanoRead >= NANO_READ_INTERVAL) {
    lastNanoRead = millis();
    readNanoSensorData();
    yield(); // Feed watchdog after I2C communication
  }
  
  // Send heartbeat periodically to confirm ESP is online
  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    lastHeartbeat = millis();
    if (mqttClient.connected()) {
      mqttClient.publish(heartbeat_topic, "alive", false);
      Serial.println("💓 Heartbeat sent");
    }
    yield(); // Feed watchdog after heartbeat
  }
  
  // Check motion sensor frequently
  if (millis() - lastMotionCheck >= MOTION_CHECK_INTERVAL) {
    lastMotionCheck = millis();
    checkMotionSensor();
  }
  
  // Check flame sensor frequently
  if (millis() - lastFlameCheck >= FLAME_CHECK_INTERVAL) {
    lastFlameCheck = millis();
    checkFlameSensor();
  }
  
  // Debug: Report ZCD interrupt rate every 30 seconds (reduced frequency)
  // NOTE: ZCD will show 0 if AC power is not connected to dimmer circuit
  if (millis() - lastZcdReport >= 30000) {
    unsigned long currentCount = zcdCounter;
    unsigned long countDiff = currentCount - lastZcdCount;
    Serial.print("⚡ ZCD: ");
    Serial.print(countDiff);
    Serial.print(" interrupts in last 30s ");
    if (countDiff == 0) {
      Serial.println("(AC dimmer not powered - normal if AC disconnected)");
    } else {
      Serial.print("(Fan1: ");
      Serial.print(fan1Power);
      Serial.print("%, Fan2: ");
      Serial.print(fan2Power);
      Serial.println("%)");
    }
    lastZcdReport = millis();
    lastZcdCount = currentCount;
  }
  
  // All the dimming magic happens in the ZCD interrupt and Ticker callbacks.
  // Sensorsc are read periodically above.
}