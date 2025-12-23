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
  delay(500);
  
  // Clear startup banner
  Serial.println(F("\n\n========================================"));
  Serial.println(F("  Arduino Nano I2C Slave v2.1"));
  Serial.println(F("  DHT22 + PZEM-004T Monitor"));
  Serial.println(F("========================================"));
  Serial.println();
  
  // Initialize I2C as slave
  Wire.begin(I2C_SLAVE_ADDRESS);
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);
  Serial.println(F("✓ I2C Slave: Address 0x08"));
  
  // Initialize PZEM-004T
  Serial.print(F("Initializing PZEM-004T... "));
  pzemSerial.begin(9600);
  delay(1000);
  
  // Test PZEM
  float testVoltage = pzem.voltage();
  delay(500);
  
  if (!isnan(testVoltage) && testVoltage > 0) {
    Serial.print(F("✓ OK: "));
    Serial.print(testVoltage);
    Serial.println(F("V"));
  } else {
    Serial.println(F("✗ FAIL!"));
    Serial.println(F("  Check: AC Power, RX/TX pins, Load connected"));
  }
  
  // Initialize DHT22
  Serial.print(F("Initializing DHT22 on D12... "));
  dht.begin();
  delay(2000);  // DHT22 needs 2 seconds to stabilize after power on
  
  // Test DHT22 immediately
  float testTemp = dht.readTemperature();
  float testHum = dht.readHumidity();
  
  if (!isnan(testTemp) && !isnan(testHum)) {
    Serial.print(F("✓ OK: "));
    Serial.print(testTemp);
    Serial.print(F("°C, "));
    Serial.print(testHum);
    Serial.println(F("%"));
  } else {
    Serial.println(F("✗ FAIL!"));
    Serial.println(F("  Wiring Check:"));
    Serial.println(F("    DHT22 VCC   -> Arduino 5V"));
    Serial.println(F("    DHT22 DATA  -> Arduino D12 (with 10K pullup to 5V)"));
    Serial.println(F("    DHT22 GND   -> Arduino GND"));
    Serial.println(F("  Note: DHT22 requires 10K pullup resistor!"));
  }
  
  // Setup pins
  pinMode(EXTRA_PIR_PIN, INPUT);
  pinMode(LIGHT1_PIN, OUTPUT);
  pinMode(LIGHT2_PIN, OUTPUT);
  pinMode(EXTRA_FAN_PIN, OUTPUT);
  pinMode(LED_INDICATOR, OUTPUT);
  
  digitalWrite(LIGHT1_PIN, LOW);
  digitalWrite(LIGHT2_PIN, LOW);
  analogWrite(EXTRA_FAN_PIN, 0);
  digitalWrite(LED_INDICATOR, HIGH);
  
  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F("  System Ready!"));
  Serial.println(F("  Monitoring DHT22 every 2 seconds"));
  Serial.println(F("  Serial Monitor: 115200 baud"));
  Serial.println(F("========================================"));
  Serial.println();
  
  // Initial PZEM read
  delay(1000);
  readPZEMData();
}

void loop() {
  // Serial commands
  if (Serial.available()) {
    char cmd = Serial.read();
    if (cmd == 't' || cmd == 'T') {
      readPZEMData();
    } else if (cmd == 'r' || cmd == 'R') {
      pzem.resetEnergy();
      Serial.println(F("Energy reset"));
    }
  }
  
  // Read DHT22 every 2 seconds (DHT22 needs time between reads)
  static unsigned long lastDhtRead = 0;
  if (millis() - lastDhtRead >= 2000) {
    lastDhtRead = millis();
    
    // Add debug timestamp
    Serial.print(F("["));
    Serial.print(millis() / 1000);
    Serial.print(F("s] Reading DHT22... "));
    
    temperature = dht.readTemperature();
    humidity = dht.readHumidity();
    
    if (!isnan(temperature) && !isnan(humidity)) {
      sensorData.temperature = (int)(temperature * 10);
      sensorData.humidity = (int)(humidity * 10);
      
      // Print DHT22 data to Serial Monitor with clear formatting
      Serial.print(F("✓ DHT22 | Temp: "));
      Serial.print(temperature, 1);
      Serial.print(F("°C | Humidity: "));
      Serial.print(humidity, 1);
      Serial.print(F("% | Raw: T="));
      Serial.print(sensorData.temperature);
      Serial.print(F(" H="));
      Serial.println(sensorData.humidity);
    } else {
      Serial.print(F("✗ FAILED | T="));
      Serial.print(temperature);
      Serial.print(F(" H="));
      Serial.println(humidity);
      Serial.println(F("   Check: DHT22 wiring, power, 10K pullup resistor on DATA"));
    }
  }
  
  sensorData.motion = digitalRead(EXTRA_PIR_PIN);
  sensorData.gasLevel = 0;
  
  // Read PZEM every 2 seconds
  static unsigned long lastPzemRead = 0;
  if (millis() - lastPzemRead > 2000) {
    lastPzemRead = millis();
    readPZEMData();
  }
  
  // Blink LED
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
  
  if (!isnan(voltage) && voltage > 0) {
    Serial.print(F("⚡ PZEM | V:"));
    Serial.print(voltage, 1);
    Serial.print(F("V | A:"));
    Serial.print(current, 2);
    Serial.print(F("A | W:"));
    Serial.print(power, 0);
    Serial.print(F("W | Hz:"));
    Serial.print(frequency, 1);
    Serial.print(F(" | PF:"));
    Serial.println(powerFactor, 2);
  } else {
    Serial.println(F("⚡ PZEM | No load or disconnected"));
  }
}

void receiveEvent(int numBytes) {
  if (numBytes >= 1) {
    command = Wire.read();
    if (numBytes >= 2) {
      commandData = Wire.read();
    }
    executeCommand(command, commandData);
  }
}

void requestEvent() {
  byte dataPacket[32];
  
  dataPacket[0] = sensorData.motion ? 1 : 0;
  dataPacket[1] = (sensorData.temperature >> 8) & 0xFF;
  dataPacket[2] = sensorData.temperature & 0xFF;
  dataPacket[3] = (sensorData.humidity >> 8) & 0xFF;
  dataPacket[4] = sensorData.humidity & 0xFF;
  dataPacket[5] = sensorData.fanSpeed;
  
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
  
  dataPacket[18] = sensorData.light1State ? 1 : 0;
  dataPacket[19] = sensorData.light2State ? 1 : 0;
  
  for (int i = 20; i < 32; i++) {
    dataPacket[i] = 0;
  }
  
  Wire.write(dataPacket, 32);
}

void executeCommand(byte cmd, byte data) {
  switch(cmd) {
    case 0x01:
      digitalWrite(LIGHT1_PIN, HIGH);
      sensorData.light1State = true;
      break;
    case 0x02:
      digitalWrite(LIGHT1_PIN, LOW);
      sensorData.light1State = false;
      break;
    case 0x03:
      digitalWrite(LIGHT2_PIN, HIGH);
      sensorData.light2State = true;
      break;
    case 0x04:
      digitalWrite(LIGHT2_PIN, LOW);
      sensorData.light2State = false;
      break;
    case 0x10:
      analogWrite(EXTRA_FAN_PIN, data);
      sensorData.fanSpeed = data;
      break;
    case 0xFF:
      digitalWrite(LIGHT1_PIN, LOW);
      digitalWrite(LIGHT2_PIN, LOW);
      analogWrite(EXTRA_FAN_PIN, 0);
      sensorData.light1State = false;
      sensorData.light2State = false;
      break;
  }
}
