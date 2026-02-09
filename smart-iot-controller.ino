#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "DHT.h"
#include <time.h>

// ===== WiFi credentials =====
const char* ssid = "vivo V15";
const char* password = "pogiako123";

// ===== Firestore URLs =====
const char* FIRESTORE_URL = "https://firestore.googleapis.com/v1/projects/smart-drying-iot/databases/(default)/documents/device_sensors/00:1A:2B:3C:4D:5E";
const char* DEVICE_ID = "00:1A:2B:3C:4D:5E";
String ACTUATOR_URL = "https://firestore.googleapis.com/v1/projects/smart-drying-iot/databases/(default)/documents/devices/" + String(DEVICE_ID);

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
const unsigned long interval = 10000; // 10 seconds

// ===== Device Vars ====
const int actuatorExtendPin = 25;  // Relay1 control
const int actuatorRetractPin = 26; // Relay2 control
const int fansPin = 13;

// Actuator states
bool isFansOpen = false;
bool isActuatorExtended = false;

// Actuator movement timing
unsigned long actuatorMovementStart = 0;
bool isActuatorMoving = false;
String targetMovement = ""; // "extend" or "retract"
const unsigned long ACTUATOR_MOVEMENT_TIME = 60000; // 60 seconds for complete movement

// Cooldown after movement completes
unsigned long actuatorCooldownStart = 0;
bool cooldownActive = false;
const unsigned long ACTUATOR_COOLDOWN_MS = 20000; // 20 seconds

// Rain detection
bool rainDetected = false;
bool rainNotificationSent = false;
int rainThreshold = 2000; // Adjust based on your sensor (lower = more rain)

// Last sensor readings (for notifications)
float lastTemp = 0;
float lastHum = 0;
int lastRainAO = 0;
int lastLightAO = 0;

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

bool isRaining(int rainAO) {
  // Lower analog value = more rain detected
  return rainAO < rainThreshold;
}

// ===== Firestore sending function =====
void sendToFirestore(float temp, float hum, int rainAO, int lightAO) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected!");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  String url = String(FIRESTORE_URL) + "?updateMask.fieldPaths=temperature" +
               "&updateMask.fieldPaths=humidity" +
               "&updateMask.fieldPaths=rainAO" +
               "&updateMask.fieldPaths=light" +
               "&updateMask.fieldPaths=updatedAt";

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to get NTP time");
    http.end();
    return;
  }

  char timestamp[30];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

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

// ===== Actuator State Update to Firestore =====
void updateActuatorState(const char* state) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected!");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  String url = ACTUATOR_URL + "?updateMask.fieldPaths=actuator.state&updateMask.fieldPaths=actuator.updatedAt";

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to get NTP time");
    http.end();
    return;
  }

  char timestamp[30];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

  StaticJsonDocument<512> doc;
  JsonObject fields = doc.createNestedObject("fields");

  // Create nested map structure for actuator field
  JsonObject actuator = fields.createNestedObject("actuator")
                              .createNestedObject("mapValue")
                              .createNestedObject("fields");

  actuator["state"]["stringValue"] = state;
  actuator["updatedAt"]["timestampValue"] = timestamp;

  String payload;
  serializeJson(doc, payload);

  Serial.println("\n--- Updating Actuator State ---");
  Serial.println("URL: " + url);
  Serial.println("State: " + String(state));
  Serial.println("Payload: " + payload);

  int httpCode = http.PATCH(payload);

  Serial.print("HTTP Response Code: ");
  Serial.println(httpCode);

  if (httpCode > 0) {
    String response = http.getString();
    Serial.println("Response: " + response);
    if (httpCode == 200) {
      Serial.println("✓ Actuator state updated successfully!");
    } else {
      Serial.println("✗ Unexpected response code");
    }
  } else {
    Serial.print("✗ Error updating actuator state: ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
  Serial.println("-------------------------------\n");
}

// ===== Main loop =====
void loop() {
  unsigned long now = millis();

  // Read all sensors
  float temp = readTemperature();
  float hum = readHumidity();
  int rainAO = readRainAO();
  int lightAO = readLightAO();

  // Store for notifications
  lastTemp = temp;
  lastHum = hum;
  lastRainAO = rainAO;
  lastLightAO = lightAO;

  // Check for rain and trigger retraction
  bool currentRainStatus = isRaining(rainAO);
  
  if (currentRainStatus && !rainDetected) {
    // Rain just started
    rainDetected = true;
    rainNotificationSent = false;
    
    Serial.println("\n!!! RAIN DETECTED !!!");
    
    // Immediately retract actuator (notification will be sent when complete)
    retractActuator();
    
  } else if (!currentRainStatus && rainDetected) {
    // Rain stopped
    rainDetected = false;
    rainNotificationSent = false;
    Serial.println("\n--- Rain cleared ---");
  }

  // Handle actuator movement state updates
  if (isActuatorMoving) {
    unsigned long elapsed = now - actuatorMovementStart;
    
    if (elapsed >= ACTUATOR_MOVEMENT_TIME) {
      // Movement complete
      isActuatorMoving = false;
      actuatorSafeTurnOff();
      
      // Update state to final position
      if (targetMovement == "extend") {
        updateActuatorState("extended");
        isActuatorExtended = true;
        Serial.println("Actuator fully extended!");
      } else if (targetMovement == "retract") {
        updateActuatorState("retracted");
        isActuatorExtended = false;
        Serial.println("Actuator fully retracted!");
        
        // Send notification ONLY if retraction was triggered by rain
        if (rainDetected && !rainNotificationSent) {
          sendNotification(
            "Drying Rack Retracted",
            "Rain detected. Drying rack fully retracted to protect your clothes.",
            "system",
            "actuator",
            "rain_sensor",
            "retract_complete",
            "high",
            lastTemp, lastHum, lastRainAO, lastLightAO
          );
          rainNotificationSent = true;
        }
      }
      
      targetMovement = "";
      
      // Start cooldown after movement completes
      startActuatorCooldown();
    } else {
      // Still moving - print progress every 10 seconds
      if (elapsed % 10000 < 100) {
        Serial.print("Actuator moving... ");
        Serial.print(elapsed / 1000);
        Serial.print("/");
        Serial.print(ACTUATOR_MOVEMENT_TIME / 1000);
        Serial.println(" seconds");
      }
    }
  }

  // Regular sensor update interval
  if (now - lastUpdate >= interval) {
    lastUpdate = now;

    // Debug print
    Serial.println("\n=== Sensor Readings ===");
    Serial.print("Temperature: ");
    Serial.print(temp);
    Serial.println(" °C");
    Serial.print("Humidity: ");
    Serial.print(hum);
    Serial.println(" %");
    Serial.print("Rain Sensor (AO): ");
    Serial.print(rainAO);
    Serial.print(" - ");
    Serial.println(currentRainStatus ? "RAINING" : "DRY");
    Serial.print("Light Sensor (AO): ");
    Serial.println(lightAO);
    Serial.println("=======================");

    // Send to Firestore
    sendToFirestore(temp, hum, rainAO, lightAO);
  }

  delay(100); // Small delay to prevent overwhelming the loop
}

// ===== Actuator Control Functions =====
void extendActuator() {
  // Check if already moving
  if (isActuatorMoving) {
    Serial.println("Actuator is currently moving. Please wait.");
    return;
  }

  // Check if cooldown is still active
  if (!isCooldownDone()) {
    unsigned long remainingTime = ACTUATOR_COOLDOWN_MS - (millis() - actuatorCooldownStart);
    Serial.print("Actuator is still in cooldown. Remaining: ");
    Serial.print(remainingTime / 1000);
    Serial.println(" seconds");
    return;
  }

  if (isActuatorExtended) {
    Serial.println("Actuator is already extended");
    return;
  }

  // Close fans first
  if (isFansOpen) {
    closeFans();
  }

  actuatorSafeTurnOff();

  // Update Firestore state to "moving_extend" FIRST
  updateActuatorState("moving_extend");

  // Start physical movement
  digitalWrite(actuatorExtendPin, HIGH);   // Relay1: COM→NO (12V+)
  digitalWrite(actuatorRetractPin, LOW);   // Relay2: COM→NC (GND)

  // Set movement tracking
  isActuatorMoving = true;
  targetMovement = "extend";
  actuatorMovementStart = millis();

  Serial.println("Actuator extending... (60 seconds)");
}

void retractActuator() {
  // Check if already moving
  if (isActuatorMoving) {
    Serial.println("Actuator is currently moving. Please wait.");
    return;
  }

  // Check if cooldown is still active
  if (!isCooldownDone()) {
    unsigned long remainingTime = ACTUATOR_COOLDOWN_MS - (millis() - actuatorCooldownStart);
    Serial.print("Actuator is still in cooldown. Remaining: ");
    Serial.print(remainingTime / 1000);
    Serial.println(" seconds");
    return;
  }

  if (!isActuatorExtended && !isActuatorMoving) {
    Serial.println("Actuator is already retracted");
    return;
  }

  actuatorSafeTurnOff();

  // Update Firestore state to "moving_retract" FIRST
  updateActuatorState("moving_retract");

  // Start physical movement
  digitalWrite(actuatorExtendPin, LOW);    // Relay1: COM→NC (GND)
  digitalWrite(actuatorRetractPin, HIGH);  // Relay2: COM→NO (12V+)

  // Set movement tracking
  isActuatorMoving = true;
  targetMovement = "retract";
  actuatorMovementStart = millis();

  Serial.println("Actuator retracting... (60 seconds)");
}

void actuatorSafeTurnOff() {
  digitalWrite(actuatorExtendPin, LOW);
  digitalWrite(actuatorRetractPin, LOW);
  delay(500); // Short delay for safety
  Serial.println("Actuator safe turn-off activated");
}

void startActuatorCooldown() {
  actuatorCooldownStart = millis();
  cooldownActive = true;
  Serial.print("Actuator cooldown started: ");
  Serial.print(ACTUATOR_COOLDOWN_MS / 1000);
  Serial.println(" seconds");
}

bool isCooldownDone() {
  if (!cooldownActive) return true;

  if (millis() - actuatorCooldownStart >= ACTUATOR_COOLDOWN_MS) {
    cooldownActive = false;
    Serial.println("Actuator cooldown completed");
    return true;
  }
  return false;
}

// ===== Fan Control Functions =====
void openFans() {
  if (isFansOpen) {
    Serial.println("Fans are already opened");
    return;
  }

  if (isActuatorExtended || isActuatorMoving) {
    Serial.println("Fans can't be activated while actuator is extended or moving");
    return;
  }

  digitalWrite(fansPin, HIGH);
  isFansOpen = true;
  Serial.println("Fans are opened");
}

void closeFans() {
  if (!isFansOpen) {
    Serial.println("Fans are already closed");
    return;
  }

  digitalWrite(fansPin, LOW);
  isFansOpen = false;
  Serial.println("Fans are closed");
}

// ===== Notification sending function =====
void sendNotification(const char* title, const char* body, const char* type,
                      const char* category, const char* trigger, const char* action,
                      const char* priority, float temp, float hum, int rainAO, int lightAO) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected!");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  String url = "https://firestore.googleapis.com/v1/projects/smart-drying-iot/databases/(default)/documents/notifications";

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to get NTP time for notification");
    http.end();
    return;
  }

  char timestamp[30];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

  StaticJsonDocument<1024> doc;
  JsonObject fields = doc.createNestedObject("fields");

  // Core Fields
  fields["title"]["stringValue"] = title;
  fields["body"]["stringValue"] = body;
  fields["time"]["timestampValue"] = timestamp;
  fields["isRead"]["booleanValue"] = false;

  // Categorization
  fields["type"]["stringValue"] = type;
  fields["category"]["stringValue"] = category;

  // Source Context
  fields["deviceId"]["stringValue"] = DEVICE_ID;
  fields["trigger"]["stringValue"] = trigger;

  // Action/State Data
  fields["action"]["stringValue"] = action;

  // Sensor Data (nested map)
  JsonObject sensorData = fields["sensorData"].createNestedObject("mapValue").createNestedObject("fields");
  sensorData["temperature"]["doubleValue"] = temp;
  sensorData["humidity"]["doubleValue"] = hum;
  sensorData["rainAO"]["integerValue"] = rainAO;
  sensorData["lightAO"]["integerValue"] = lightAO;

  // Metadata
  fields["priority"]["stringValue"] = priority;
  fields["acknowledged"]["booleanValue"] = false;
  fields["createdAt"]["timestampValue"] = timestamp;

  String payload;
  serializeJson(doc, payload);

  Serial.println("\n--- Sending Notification to Firestore ---");
  Serial.println("URL: " + url);
  Serial.println("Payload: " + payload);

  int httpCode = http.POST(payload);

  Serial.print("HTTP Response Code: ");
  Serial.println(httpCode);

  if (httpCode > 0) {
    String response = http.getString();
    Serial.println("Response: " + response);
    if (httpCode == 200) {
      Serial.println("✓ Notification sent to Firestore successfully!");
    } else {
      Serial.println("✗ Unexpected response code");
    }
  } else {
    Serial.print("✗ Error sending notification: ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
  Serial.println("----------------------------------------\n");
}
