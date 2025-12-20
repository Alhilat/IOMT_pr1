#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <SPI.h>
#include <SD.h>

// ================= CONFIGURATION =================
const char* WIFI_SSID = "A";        
const char* WIFI_PASS = "12487fgh";    
String GOOGLE_SCRIPT_ID = "AKfycbwGq4ITi13n3Hs8m_eKaFbI3MgsCgobmMAJdo0KuWEwPrsva1YsWCqNk3RiDSLvqppF"; 

// ================= PINS =================
#define DHTPIN        4     
#define DHTTYPE       DHT11
#define ONE_WIRE_BUS  15    
#define BUZZER_PIN    5 
#define SD_CS_PIN     27  

// ================= OBJECTS =================
Adafruit_SSD1306 display(128, 64, &Wire, -1);
DHT dht(DHTPIN, DHTTYPE);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
MAX30105 particleSensor;

// ================= GLOBALS =================
bool sdAvailable = false;
const int CHECK_INTERVAL_MINUTES = 10;
const long FINGER_THRESHOLD = 7000;

// SpO2 logic vars
uint32_t irBuffer[50];
uint32_t redBuffer[50];
int bufferIndex = 0;
long lastBeat = 0;

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  
  // 1. Screen Init
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.setTextColor(SSD1306_WHITE);
  display.clearDisplay();

  // 2. Sensors Init
  pinMode(BUZZER_PIN, OUTPUT);
  dht.begin();
  sensors.begin();
  
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 failed");
    while(1);
  }
  particleSensor.setup(0x1F, 4, 2, 400, 411, 4096); 

  // 3. SD Card Init
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD Failed");
    sdAvailable = false;
  } else {
    sdAvailable = true;
    if(!SD.exists("/health_data.csv")) {
      File file = SD.open("/health_data.csv", FILE_WRITE);
      if(file) {
        file.println("Millis,BodyTemp,HR,SpO2,RoomTemp,Humidity");
        file.close();
      }
    }
  }

  // 4. WiFi Init
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  // We don't wait here, we check connection before uploading
}

// ================= MAIN LOOP (LINEAR FLOW) =================
void loop() {
  
  // STEP 1: Wait for the interval (Blocking delay with countdown)
  waitForNextScan(CHECK_INTERVAL_MINUTES);

  // STEP 2: Alert User
  triggerAlert();

  // STEP 3: Measure Data (Runs for 15 seconds or until stable)
  // We pass variables by reference (&) to fill them with data
  float bodyT = 0, roomT = 0, hum = 0;
  int hr = 0, o2 = 0;
  
  bool success = performMeasurement(bodyT, roomT, hum, hr, o2);

  // STEP 4: Save & Upload (Only if measurement was good)
  if (success) {
    saveToSD(bodyT, hr, o2, roomT, hum);
    uploadToCloud(bodyT, hr, o2, roomT, hum);
  } else {
    display.clearDisplay();
    display.setCursor(10, 30); display.println("Scan Failed/Skip");
    display.display();
    delay(2000);
  }
}

// ================= SIMPLIFIED FUNCTIONS =================

void waitForNextScan(int minutes) {
  unsigned long duration = minutes * 60 * 1000;
  unsigned long startTime = millis();
  
  while (millis() - startTime < duration) {
    long remainingSeconds = (duration - (millis() - startTime)) / 1000;
    
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(20, 10); display.println("Next Check In:");
    display.setTextSize(2);
    display.setCursor(35, 30); 
    display.print(remainingSeconds / 60); 
    display.print(":");
    if ((remainingSeconds % 60) < 10) display.print("0");
    display.print(remainingSeconds % 60);
    display.display();
    
    delay(200); // Small delay to stop screen flickering
  }
}

void triggerAlert() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(10, 25); display.println("CHECK NOW!");
  display.display();
  
  for(int i=0; i<3; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(200);
    digitalWrite(BUZZER_PIN, LOW);  delay(100);
  }
}

// This function runs an internal loop for 15 seconds to catch the heartbeat
bool performMeasurement(float &body, float &room, float &hum, int &finalHR, int &finalO2) {
  long startMeasure = millis();
  int beatAvg = 0;
  byte rates[4];
  byte rateSpot = 0;
  int validSpO2 = 0;
  
  bufferIndex = 0; // Reset buffer

  // Run this loop for 20 seconds MAX
  while (millis() - startMeasure < 20000) {
    
    // 1. Read Particle Sensor (Must happen fast!)
    long irValue = particleSensor.getIR();
    long redValue = particleSensor.getRed();

    if (irValue < FINGER_THRESHOLD) {
       display.clearDisplay();
       display.setTextSize(1);
       display.setCursor(25, 30); display.println("Place Finger...");
       display.display();
       bufferIndex = 0; // Reset calculation if finger lifted
    } else {
       // Heart Rate Logic
       if (checkForBeat(irValue) == true) {
         long delta = millis() - lastBeat;
         lastBeat = millis();
         float bpm = 60 / (delta / 1000.0);
         if (bpm < 255 && bpm > 20) {
           rates[rateSpot++] = (byte)bpm;
           rateSpot %= 4;
           beatAvg = 0;
           for (byte x = 0 ; x < 4 ; x++) beatAvg += rates[x];
           beatAvg /= 4;
         }
       }
       
       // Fill Buffer for SpO2
       irBuffer[bufferIndex] = irValue;
       redBuffer[bufferIndex] = redValue;
       bufferIndex++;
       if (bufferIndex >= 50) {
         calculateSpO2(validSpO2); // Helper to calculate
         bufferIndex = 0;
       }

       // Display Live Data
       display.clearDisplay();
       display.setCursor(0,0); display.print("Measuring...");
       display.setCursor(0,20); display.print("HR: "); display.print(beatAvg);
       display.setCursor(0,40); display.print("O2: "); display.print(validSpO2); display.print("%");
       display.display();

       // Check if Stable (Exit condition)
       // If we have valid data for > 2 seconds (roughly), we capture and exit
       if (beatAvg > 40 && validSpO2 > 80 && bufferIndex > 40) {
          // Capture other sensors now
          room = dht.readTemperature();
          hum = dht.readHumidity();
          sensors.requestTemperatures();
          body = sensors.getTempCByIndex(0);
          if(body == -127) body = 0;

          finalHR = beatAvg;
          finalO2 = validSpO2;
          
          display.clearDisplay();
          display.setCursor(20, 30); display.println("Done!"); 
          display.display();
          delay(1000);
          return true; // Success
       }
    }
  }
  return false; // Timed out
}

void saveToSD(float body, int hr, int o2, float room, float hum) {
  display.clearDisplay();
  display.setCursor(0,0); display.println("Saving SD...");
  display.display();
  
  if (sdAvailable) {
    File file = SD.open("/health_data.csv", FILE_APPEND);
    if (file) {
      file.print(millis()); file.print(",");
      file.print(body); file.print(",");
      file.print(hr); file.print(",");
      file.print(o2); file.print(",");
      file.print(room); file.print(",");
      file.println(hum);
      file.close();
      display.println("SD Saved.");
    } else {
      display.println("SD Error.");
    }
  } else {
    display.println("No SD.");
  }
  display.display();
  delay(1000);
}

void uploadToCloud(float body, int hr, int o2, float room, float hum) {
  display.clearDisplay();
  display.setCursor(0,0); display.println("Uploading...");
  display.display();

  // Connect WiFi if needed
  if(WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(); WiFi.reconnect();
    long waitStart = millis();
    while(WiFi.status() != WL_CONNECTED && millis() - waitStart < 5000) { delay(100); }
  }

  if(WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "https://script.google.com/macros/s/" + GOOGLE_SCRIPT_ID + "/exec?";
    url += "bodyT=" + String(body);
    url += "&hr=" + String(hr);
    url += "&o2=" + String(o2);
    url += "&roomT=" + String(room);
    url += "&hum=" + String(hum); 
    
    http.begin(url);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    int httpCode = http.GET();
    
    display.setCursor(0, 20);
    if (httpCode > 0) display.println("Sent OK!");
    else display.println("Sent Fail!");
    http.end();
  } else {
    display.setCursor(0, 20); display.println("No WiFi!");
  }
  display.display();
  delay(2000);
}

void calculateSpO2(int &spo2Val) {
  uint32_t minIR = 999999, maxIR = 0, minRed = 999999, maxRed = 0;
  for (int i=0; i<50; i++) {
    if (irBuffer[i] < minIR) minIR = irBuffer[i];
    if (irBuffer[i] > maxIR) maxIR = irBuffer[i];
    if (redBuffer[i] < minRed) minRed = redBuffer[i];
    if (redBuffer[i] > maxRed) maxRed = redBuffer[i];
  }
  float acIR = maxIR - minIR; float dcIR = (maxIR + minIR) / 2;
  float acRed = maxRed - minRed; float dcRed = (maxRed + minRed) / 2;
  float R = (acRed / dcRed) / (acIR / dcIR);
  float reading = 104 - (17 * R);
  if (reading > 100) reading = 100;
  if (reading < 70) reading = 0; 
  spo2Val = (int)reading;
}