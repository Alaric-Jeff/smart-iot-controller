#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "DHT.h"
#include <time.h>

// ===== WiFi credentials =====
const char* ssid = "vivo V15";
const char* password = "pogiako123";

// ===== Firestore URL =====
const char* FIRESTORE_URL = "https://firestore.googleapis.com/v1/projects/smart-drying-iot/databases/(default)/documents/device_sensors/00:1A:2B:3C:4D:5E";

// ===== Sensor pins =====
#define DHTPIN 23
#define DHTTYPE DHT22
#define RAIN_AO 35
#define RAIN_DO 22
#define LIGHT_AO 33
#define LIGHT_DO 16

DHT dht(DHTPIN, DHTTYPE);

// ===== Timing =====
unsigned long lastUpdate = 0;
const unsigned long interval = 10000;  // 10 seconds

// ===== Device Vars ====

const int actuatorExtendPin = 25;
const int actuatorRetractPin = 26;
const int fansPin = 24;

// Initialized states
bool isFansOpen;
bool isActuatorExtended;

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  dht.begin();

  pinMode(actuatorExtendPin, OUTPUT);
  pinMode(actuatorRetractPin, OUTPUT);
  actuatorSafeTurnOff();

  isActuatorExtended = false;

  pinMode(fansPin, OUTPUT);
  isFansOpen = false;
  digitalWrite(fansPin, LOW);

  pinMode(RAIN_DO, INPUT);
  pinMode(LIGHT_DO, INPUT);

  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Init NTP for timestamps (UTC)
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  // Wait for time sync
  Serial.print("Waiting for NTP time sync");
  delay(2000);

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    Serial.println("\nTime synced successfully");
  } else {
    Serial.println("\nFailed to sync time");
  }

  Serial.println("ESP32 + DHT22 + Rain + Light Sensor started...");
}

// ===== Sensor reading functions =====
float readTemperature() {
  float t = dht.readTemperature();
  return isnan(t) ? -1.0 : t;
}

float readHumidity() {
  float h = dht.readHumidity();
  return isnan(h) ? -1.0 : h;
}

int readRainAO() {
  return analogRead(RAIN_AO);
}

int readLightAO() {
  return analogRead(LIGHT_AO);
}

// ===== Firestore sending function =====
void sendToFirestore(float temp, float hum, int rainAO, int lightAO) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected!");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();  // Skip certificate verification for demo

  HTTPClient http;

  // Add updateMask query parameters
  String url = String(FIRESTORE_URL) + "?updateMask.fieldPaths=temperature" + "&updateMask.fieldPaths=humidity" + "&updateMask.fieldPaths=rainAO" + "&updateMask.fieldPaths=light" + "&updateMask.fieldPaths=updatedAt";

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  // Get current UTC time for Firestore timestamp
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to get NTP time");
    http.end();
    return;
  }

  char timestamp[30];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

  // Build JSON payload (NO "name" field)
  StaticJsonDocument<512> doc;
  JsonObject fields = doc.createNestedObject("fields");

  fields["temperature"]["doubleValue"] = temp;
  fields["humidity"]["doubleValue"] = hum;
  fields["rainAO"]["integerValue"] = rainAO;
  fields["light"]["integerValue"] = lightAO;
  fields["updatedAt"]["timestampValue"] = timestamp;

  String payload;
  serializeJson(doc, payload);

  Serial.println("\n--- Sending to Firestore ---");
  Serial.println("URL: " + url);
  Serial.println("Payload: " + payload);

  // Use PATCH to update existing document
  int httpCode = http.PATCH(payload);

  Serial.print("HTTP Response Code: ");
  Serial.println(httpCode);

  if (httpCode > 0) {
    String response = http.getString();
    Serial.println("Response: " + response);

    if (httpCode == 200) {
      Serial.println("✓ Data sent to Firestore successfully!");
    } else {
      Serial.println("✗ Unexpected response code");
    }
  } else {
    Serial.print("✗ Error sending to Firestore: ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
  Serial.println("----------------------------\n");
}

// ===== Main loop =====
void loop() {
  unsigned long now = millis();

  if (now - lastUpdate >= interval) {
    lastUpdate = now;

    // Read all sensors
    float temp = readTemperature();
    float hum = readHumidity();
    int rainAO = readRainAO();
    int lightAO = readLightAO();

    // Debug print
    Serial.println("\n=== Sensor Readings ===");
    Serial.print("Temperature: ");
    Serial.print(temp);
    Serial.println(" °C");

    Serial.print("Humidity: ");
    Serial.print(hum);
    Serial.println(" %");

    Serial.print("Rain Sensor (AO): ");
    Serial.println(rainAO);

    Serial.print("Light Sensor (AO): ");
    Serial.println(lightAO);
    Serial.println("=======================");

    // Send to Firestore
    sendToFirestore(temp, hum, rainAO, lightAO);
  }
}


void extendActuator() {
  if (isActuatorExtended == true) {
    Serial.println("Actuator is already extended");
    return;
  }

  actuatorSafeTurnOff();

  digitalWrite(actuatorExtendPin, HIGH);
  isActuatorExtended = true;
  return;
}

void retractActuator() {
  if (isActuatorExtended == false) {
    Serial.println("Actuator is already retracted");
    return;
  }

  actuatorSafeTurnOff();

  digitalWrite(actuatorRetractPin, HIGH); 
  isActuatorExtended = false;
  return;
}

void actuatorSafeTurnOff(){
  digitalWrite(actuatorExtendPin, LOW);
  digitalWrite(actuatorRetractPin, LOW);
  delay(500);
  return;
}

void openFans() {
  if (isFansOpen == true) {
    Serial.println("Fans is already opened");
    return;
  }
  digitalWrite(fansPin, HIGH);
  isFansOpen = true;
  return;
}

void closeFans() {
  if (isFansOpen == false) {
    Serial.println("Fans is already closed");
    return;
  }
  digitalWrite(fansPin, LOW);
  isFansOpen = false;
  return;
}
