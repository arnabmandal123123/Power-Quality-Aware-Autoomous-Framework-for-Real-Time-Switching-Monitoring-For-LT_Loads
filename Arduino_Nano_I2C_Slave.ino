/*
 * Arduino Nano - I2C Slave for ESP8266
 * Acts as I/O expansion for ESP8266
 * Handles additional sensors and devices
 * Now includes PZEM-004T AC power monitoring + DHT22 sensor
 */

#include <Wire.h>
#include <PZEM004Tv30.h>
#include <SoftwareSerial.h>
#include <DHT.h>

// PZEM-004T Software Serial pins
#define PZEM_RX_PIN        10  // Connect to PZEM TX
#define PZEM_TX_PIN        11  // Connect to PZEM RX

// DHT22 Sensor pin
#define DHT_PIN            12  // DHT22 data pin
#define DHTTYPE            DHT22

// I2C Slave Address
#define I2C_SLAVE_ADDRESS 0x08

// Pin Definitions - Controlling ESP's D1/D2 lights via Nano
#define LIGHT1_PIN         3   // Light 1 (was ESP D1/GPIO5)
#define LIGHT2_PIN         4   // Light 2 (was ESP D2/GPIO4)
#define EXTRA_PIR_PIN      2   // Additional PIR sensor
#define EXTRA_FAN_PIN      5   // Extra fan (PWM capable)
#define LED_INDICATOR      13  // Built-in LED for status

// PZEM-004T instance
SoftwareSerial pzemSerial(PZEM_RX_PIN, PZEM_TX_PIN);
PZEM004Tv30 pzem(pzemSerial);

// DHT22 instance
DHT dht(DHT_PIN, DHTTYPE);

// PZEM data variables
float voltage = 0.0;
float current = 0.0;
float power = 0.0;
float energy = 0.0;
float frequency = 0.0;
float powerFactor = 0.0;

// DHT22 data variables
float temperature = 0.0;
float humidity = 0.0;

// Sensor data structure
struct SensorData {
  bool motion;
  int temperature;  // Now from DHT22
  int humidity;     // Added humidity from DHT22
  int gasLevel;     // Not used (set to 0)
  bool light1State;
  bool light2State;
  int fanSpeed;
};

SensorData sensorData;

// Command from ESP8266
byte command = 0;
byte commandData = 0;

void setup() {
  Serial.begin(115200);
  Serial.println("Arduino Nano I2C Slave Starting...");
  Serial.println("With PZEM-004T AC Power Monitor");
  
  // Initialize I2C as slave
  Wire.begin(I2C_SLAVE_ADDRESS);
  Wire.onReceive(receiveEvent);  // Register receive event
  Wire.onRequest(requestEvent);  // Register request event
  
  // Initialize PZEM-004T
  pzemSerial.begin(9600);
  delay(1000);
  Serial.println("✓ PZEM-004T initialized on D10(RX), D11(TX)");
  
  // Initialize DHT22
  dht.begin();
  delay(2000); // DHT22 needs 2 seconds to stabilize
  Serial.println("✓ DHT22 initialized on D12");
  
  // Setup pins
  pinMode(EXTRA_PIR_PIN, INPUT);
  pinMode(LIGHT1_PIN, OUTPUT);
  pinMode(LIGHT2_PIN, OUTPUT);
  pinMode(EXTRA_FAN_PIN, OUTPUT);
  pinMode(LED_INDICATOR, OUTPUT);
  
  // Initialize outputs
  digitalWrite(LIGHT1_PIN, LOW);
  digitalWrite(LIGHT2_PIN, LOW);
  analogWrite(EXTRA_FAN_PIN, 0);
  
  Serial.println("✓ Nano ready as I2C slave at address 0x08");
  Serial.println("✓ Controlling Light 1 (D3) and Light 2 (D4)");
  digitalWrite(LED_INDICATOR, HIGH);
}

void loop() {
  // Read DHT22 sensor
  temperature = dht.readTemperature();
  humidity = dht.readHumidity();
  
  // Check if reading failed
  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("⚠️ DHT22 read error");
    // Keep last valid values
  } else {
    sensorData.temperature = (int)(temperature * 10); // Send as integer * 10 (e.g., 25.5°C = 255)
    sensorData.humidity = (int)(humidity * 10);       // Send as integer * 10 (e.g., 60.2% = 602)
  }
  
  // Read other sensors
  sensorData.motion = digitalRead(EXTRA_PIR_PIN);
  sensorData.gasLevel = 0; // Not using extra gas sensor on Nano
  
  // Read PZEM-004T data (every 2 seconds to avoid flooding)
  static unsigned long lastPzemRead = 0;
  if (millis() - lastPzemRead > 2000) {
    lastPzemRead = millis();
    readPZEMData();
  }
  
  // Blink LED to show activity
  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > 1000) {
    digitalWrite(LED_INDICATOR, !digitalRead(LED_INDICATOR));
    lastBlink = millis();
  }
  
  delay(100);
}

// Read PZEM-004T power monitoring data
void readPZEMData() {
  voltage = pzem.voltage();
  current = pzem.current();
  power = pzem.power();
  energy = pzem.energy();
  frequency = pzem.frequency();
  powerFactor = pzem.pf();
  
  // Check if data is valid
  if (!isnan(voltage)) {
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.println("⚡ PZEM-004T Power Monitor:");
    Serial.print("  Voltage: "); Serial.print(voltage); Serial.println(" V");
    Serial.print("  Current: "); Serial.print(current); Serial.println(" A");
    Serial.print("  Power: "); Serial.print(power); Serial.println(" W");
    Serial.print("  Energy: "); Serial.print(energy); Serial.println(" kWh");
    Serial.print("  Frequency: "); Serial.print(frequency); Serial.println(" Hz");
    Serial.print("  Power Factor: "); Serial.println(powerFactor);
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━");
  } else {
    Serial.println("⚠️ PZEM: No data or not connected");
  }
}

// Called when ESP8266 sends data to Nano
void receiveEvent(int numBytes) {
  if (numBytes >= 1) {
    command = Wire.read();  // First byte is command
    
    if (numBytes >= 2) {
      commandData = Wire.read();  // Second byte is data
    }
    
    executeCommand(command, commandData);
  }
}

// Called when ESP8266 requests data from Nano
void requestEvent() {
  // Send sensor data + PZEM data as bytes (32 bytes total)
  // Packet structure:
  // [0] Motion, [1-2] Temp, [3-4] Humidity (changed from Gas), [5] FanSpeed
  // [6-9] Voltage, [10-13] Current, [14-17] Power
  // [18-21] Energy, [22-25] Frequency, [26-29] PowerFactor
  // [30-31] Reserved
  
  byte dataPacket[32];
  
  // Basic sensor data (6 bytes)
  dataPacket[0] = sensorData.motion ? 1 : 0;
  dataPacket[1] = (sensorData.temperature >> 8) & 0xFF;  // High byte (temp * 10)
  dataPacket[2] = sensorData.temperature & 0xFF;         // Low byte
  dataPacket[3] = (sensorData.humidity >> 8) & 0xFF;     // High byte (humidity * 10)
  dataPacket[4] = sensorData.humidity & 0xFF;            // Low byte
  dataPacket[5] = sensorData.fanSpeed;
  
  // PZEM data as integers (to avoid float transmission issues)
  // Multiply by 100 to keep 2 decimal places
  int voltageInt = (int)(voltage * 100);
  int currentInt = (int)(current * 100);
  int powerInt = (int)(power);
  int energyInt = (int)(energy * 100);
  int frequencyInt = (int)(frequency * 100);
  int pfInt = (int)(powerFactor * 100);
  
  dataPacket[6] = (voltageInt >> 8) & 0xFF;
  dataPacket[7] = voltageInt & 0xFF;
  dataPacket[8] = (currentInt >> 8) & 0xFF;
  dataPacket[9] = currentInt & 0xFF;
  dataPacket[10] = (powerInt >> 8) & 0xFF;
  dataPacket[11] = powerInt & 0xFF;
  dataPacket[12] = (energyInt >> 8) & 0xFF;
  dataPacket[13] = energyInt & 0xFF;
  dataPacket[14] = (frequencyInt >> 8) & 0xFF;
  dataPacket[15] = frequencyInt & 0xFF;
  dataPacket[16] = (pfInt >> 8) & 0xFF;
  dataPacket[17] = pfInt & 0xFF;
  
  // Light states
  dataPacket[18] = sensorData.light1State ? 1 : 0;
  dataPacket[19] = sensorData.light2State ? 1 : 0;
  
  // Fill remaining with zeros
  for (int i = 20; i < 32; i++) {
    dataPacket[i] = 0;
  }
  
  Wire.write(dataPacket, 32);
  Serial.println("📤 Sent data to ESP8266 (including PZEM)");
}

// Execute commands from ESP8266
void executeCommand(byte cmd, byte data) {
  Serial.print("📥 Received command: 0x");
  Serial.print(cmd, HEX);
  Serial.print(" Data: ");
  Serial.println(data);
  
  switch(cmd) {
    case 0x01:  // Light 1 ON
      digitalWrite(LIGHT1_PIN, HIGH);
      sensorData.light1State = true;
      Serial.println("✓ Light 1 ON (D3)");
      break;
      
    case 0x02:  // Light 1 OFF
      digitalWrite(LIGHT1_PIN, LOW);
      sensorData.light1State = false;
      Serial.println("✓ Light 1 OFF (D3)");
      break;
      
    case 0x03:  // Light 2 ON
      digitalWrite(LIGHT2_PIN, HIGH);
      sensorData.light2State = true;
      Serial.println("✓ Light 2 ON (D4)");
      break;
      
    case 0x04:  // Light 2 OFF
      digitalWrite(LIGHT2_PIN, LOW);
      sensorData.light2State = false;
      Serial.println("✓ Light 2 OFF (D4)");
      break;
      
    case 0x10:  // Set Fan Speed (0-255)
      analogWrite(EXTRA_FAN_PIN, data);
      sensorData.fanSpeed = data;
      Serial.print("✓ Fan speed set to: ");
      Serial.println(data);
      break;
      
    case 0x20:  // Request sensor data (handled by requestEvent)
      Serial.println("📊 Sensor data requested");
      break;
      
    case 0xFF:  // Reset/Test command
      Serial.println("🔄 Reset command received");
      digitalWrite(LIGHT1_PIN, LOW);
      digitalWrite(LIGHT2_PIN, LOW);
      analogWrite(EXTRA_FAN_PIN, 0);
      sensorData.light1State = false;
      sensorData.light2State = false;
      break;
      
    default:
      Serial.println("⚠️ Unknown command");
      break;
  }
}
