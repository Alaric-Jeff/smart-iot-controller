#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "DHT.h"
#include <time.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

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
unsigned long lastActuatorPoll = 0;
const unsigned long actuatorPollInterval = 3000; // 3 seconds

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
String lastKnownTarget = ""; // Track last known target from Firestore to detect changes

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
  // Disable brownout detector temporarily during WiFi init
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  
  Serial.begin(115200);
  delay(1000); // Allow power to stabilize
  
  dht.begin();

  // Configure all pins with outputs set to LOW to minimize power draw
  pinMode(actuatorExtendPin, OUTPUT);
  pinMode(actuatorRetractPin, OUTPUT);
  digitalWrite(actuatorExtendPin, LOW);
  digitalWrite(actuatorRetractPin, LOW);
  isActuatorExtended = false;

  pinMode(fansPin, OUTPUT);
  digitalWrite(fansPin, LOW);
  isFansOpen = false;

  pinMode(RAIN_DO, INPUT);
  pinMode(LIGHT_DO, INPUT);

  // Additional delay before WiFi init
  delay(500);
  
  Serial.println("Connecting to WiFi...");
  
  // Reduce WiFi transmission power to prevent current spike
  WiFi.setTxPower(WIFI_POWER_15dBm); // Reduced from default 20dBm
  
  // Start WiFi connection
  WiFi.begin(ssid, password);
  
  // Wait for connection with timeout
  int wifi_retry = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_retry < 40) { // 20 second timeout
    delay(500);
    Serial.print(".");
    wifi_retry++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Signal Strength (RSSI): ");
    Serial.println(WiFi.RSSI());
  } else {
    Serial.println("\nWiFi connection failed! Restarting...");
    delay(3000);
    ESP.restart();
  }

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
// FIXED: Now includes source field and uses proper updateMask
void updateActuatorState(const char* state, const char* source) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected!");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  // FIXED: Added source to updateMask
  String url = ACTUATOR_URL + "?updateMask.fieldPaths=actuator.state"
                             "&updateMask.fieldPaths=actuator.updatedAt"
                             "&updateMask.fieldPaths=actuator.source";

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
  actuator["source"]["stringValue"] = source; // ADDED: source field

  String payload;
  serializeJson(doc, payload);

  Serial.println("\n--- Updating Actuator State ---");
  Serial.println("URL: " + url);
  Serial.println("State: " + String(state));
  Serial.println("Source: " + String(source)); // ADDED: log source
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

// ===== Poll Actuator Target from Firestore =====
void pollActuatorTarget() {
  Serial.println("\n========================================");
  Serial.println("[POLL] Starting actuator target poll...");
  Serial.print("[POLL] Current time (millis): ");
  Serial.println(millis());
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[POLL] ERROR: WiFi not connected!");
    Serial.println("========================================\n");
    return;
  }
  Serial.println("[POLL] WiFi Status: Connected");

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  String url = ACTUATOR_URL;
  Serial.println("[POLL] URL: " + url);

  Serial.println("[POLL] Initializing HTTP client...");
  http.begin(client, url);

  Serial.println("[POLL] Sending GET request to Firestore...");
  int httpCode = http.GET();
  Serial.print("[POLL] HTTP Response Code: ");
  Serial.println(httpCode);

  if (httpCode == 200) {
    Serial.println("[POLL] SUCCESS: Received data from Firestore");
    String response = http.getString();
    Serial.println("[POLL] Response length: " + String(response.length()) + " bytes");
    Serial.println("[POLL] Full Response:");
    Serial.println(response);
    Serial.println("[POLL] --- End of Response ---");
    
    // Parse JSON response
    Serial.println("[POLL] Parsing JSON...");
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, response);
    
    if (error) {
      Serial.print("[POLL] ERROR: Failed to parse JSON: ");
      Serial.println(error.c_str());
      http.end();
      Serial.println("========================================\n");
      return;
    }
    Serial.println("[POLL] JSON parsed successfully");

    // Extract target value from nested structure
    // Path: fields.actuator.mapValue.fields.target.stringValue
    Serial.println("[POLL] Extracting target field...");
    if (doc["fields"]["actuator"]["mapValue"]["fields"]["target"]["stringValue"]) {
      String newTarget = doc["fields"]["actuator"]["mapValue"]["fields"]["target"]["stringValue"].as<String>();
      
      Serial.println("[POLL] ✓ Target field found!");
      Serial.print("[POLL] Polled actuator target: ");
      Serial.println(newTarget);
      Serial.print("[POLL] Last known target: ");
      Serial.println(lastKnownTarget);

      // Check if target changed
      if (newTarget != lastKnownTarget && newTarget.length() > 0) {
        Serial.println("[POLL] ╔════════════════════════════════════╗");
        Serial.println("[POLL] ║  TARGET CHANGE DETECTED!!!         ║");
        Serial.println("[POLL] ╚════════════════════════════════════╝");
        Serial.print("[POLL] Previous target: '");
        Serial.print(lastKnownTarget);
        Serial.println("'");
        Serial.print("[POLL] New target: '");
        Serial.print(newTarget);
        Serial.println("'");
        
        // Update last known target
        lastKnownTarget = newTarget;
        Serial.println("[POLL] Last known target updated");
        
        // Execute action based on new target (with "user" source since it came from mobile)
        if (newTarget == "extend") {
          Serial.println("[POLL] ►►► Executing: EXTEND (triggered by user) ◄◄◄");
          extendActuatorFromUser();
        } else if (newTarget == "retract") {
          Serial.println("[POLL] ►►► Executing: RETRACT (triggered by user) ◄◄◄");
          retractActuatorFromUser();
        } else {
          Serial.print("[POLL] WARNING: Unknown target value: ");
          Serial.println(newTarget);
        }
      } else if (lastKnownTarget.length() == 0) {
        // First poll, just store the value
        lastKnownTarget = newTarget;
        Serial.println("[POLL] This is the first poll - storing initial target state");
        Serial.print("[POLL] Initial target state: '");
        Serial.print(newTarget);
        Serial.println("'");
      } else {
        Serial.println("[POLL] No change detected - target remains: " + newTarget);
      }
    } else {
      Serial.println("[POLL] WARNING: No target field found in actuator data");
      Serial.println("[POLL] Checking document structure...");
      if (!doc["fields"]) {
        Serial.println("[POLL] ERROR: 'fields' object not found");
      } else if (!doc["fields"]["actuator"]) {
        Serial.println("[POLL] ERROR: 'actuator' field not found");
      } else if (!doc["fields"]["actuator"]["mapValue"]) {
        Serial.println("[POLL] ERROR: 'mapValue' not found in actuator");
      } else if (!doc["fields"]["actuator"]["mapValue"]["fields"]) {
        Serial.println("[POLL] ERROR: 'fields' not found in actuator.mapValue");
      } else {
        Serial.println("[POLL] ERROR: 'target' not found in actuator fields");
      }
    }
  } else {
    Serial.println("[POLL] ERROR: Failed to poll actuator state");
    Serial.print("[POLL] HTTP Code: ");
    Serial.println(httpCode);
    if (httpCode < 0) {
      Serial.print("[POLL] Error description: ");
      Serial.println(http.errorToString(httpCode));
    }
  }

  http.end();
  Serial.println("[POLL] HTTP connection closed");
  Serial.println("========================================\n");
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
    
    // Immediately retract actuator with rain_sensor source
    retractActuatorFromRain();
    
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
      Serial.println("\n[MOVEMENT] Movement time completed!");
      isActuatorMoving = false;
      actuatorSafeTurnOff();
      
      // Determine source for final state update
      const char* source = rainDetected ? "rain_sensor" : "user";
      
      // Update state to final position
      if (targetMovement == "extend") {
        Serial.println("[MOVEMENT] Updating state to 'extended'");
        updateActuatorState("extended", source);
        isActuatorExtended = true;
        Serial.println("[MOVEMENT] ✓ Actuator fully extended!");
      } else if (targetMovement == "retract") {
        Serial.println("[MOVEMENT] Updating state to 'retracted'");
        updateActuatorState("retracted", source);
        isActuatorExtended = false;
        Serial.println("[MOVEMENT] ✓ Actuator fully retracted!");
        
        // Send notification ONLY if retraction was triggered by rain
        if (rainDetected && !rainNotificationSent) {
          Serial.println("[MOVEMENT] Sending rain notification...");
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
        Serial.print("[MOVEMENT] Actuator moving... ");
        Serial.print(elapsed / 1000);
        Serial.print("/");
        Serial.print(ACTUATOR_MOVEMENT_TIME / 1000);
        Serial.println(" seconds");
      }
    }
  }

  // Poll actuator target from Firestore every 3 seconds (non-blocking)
  if (now - lastActuatorPoll >= actuatorPollInterval) {
    lastActuatorPoll = now;
    Serial.print("\n[LOOP] Time to poll actuator target (every ");
    Serial.print(actuatorPollInterval / 1000);
    Serial.println(" seconds)");
    pollActuatorTarget();
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

// ADDED: Separate function for user-triggered extend
void extendActuatorFromUser() {
  Serial.println("\n[EXTEND] extendActuatorFromUser() called");
  extendActuatorInternal("user");
}

// ADDED: Separate function for rain-triggered retract
void retractActuatorFromRain() {
  Serial.println("\n[RETRACT] retractActuatorFromRain() called");
  retractActuatorInternal("rain_sensor");
}

// ADDED: Separate function for user-triggered retract
void retractActuatorFromUser() {
  Serial.println("\n[RETRACT] retractActuatorFromUser() called");
  retractActuatorInternal("user");
}

// MODIFIED: Internal extend function with source parameter
void extendActuatorInternal(const char* source) {
  Serial.println("[EXTEND] extendActuatorInternal() executing...");
  Serial.print("[EXTEND] Source: ");
  Serial.println(source);
  
  // Check if already moving
  if (isActuatorMoving) {
    Serial.println("[EXTEND] BLOCKED: Actuator is currently moving. Please wait.");
    return;
  }

  // Check if cooldown is still active
  if (!isCooldownDone()) {
    unsigned long remainingTime = ACTUATOR_COOLDOWN_MS - (millis() - actuatorCooldownStart);
    Serial.print("[EXTEND] BLOCKED: Actuator is still in cooldown. Remaining: ");
    Serial.print(remainingTime / 1000);
    Serial.println(" seconds");
    return;
  }

  if (isActuatorExtended) {
    Serial.println("[EXTEND] BLOCKED: Actuator is already extended");
    return;
  }

  // Close fans first
  if (isFansOpen) {
    Serial.println("[EXTEND] Closing fans first...");
    closeFans();
  }

  Serial.println("[EXTEND] Calling actuatorSafeTurnOff()...");
  actuatorSafeTurnOff();

  // Update Firestore state to "moving_extend" with source
  Serial.println("[EXTEND] Updating Firestore state to 'moving_extend'...");
  updateActuatorState("moving_extend", source);

  // Start physical movement
  Serial.println("[EXTEND] Starting physical movement...");
  digitalWrite(actuatorExtendPin, HIGH);   // Relay1: COM→NO (12V+)
  digitalWrite(actuatorRetractPin, LOW);   // Relay2: COM→NC (GND)
  Serial.println("[EXTEND] Relay pins set: ExtendPin=HIGH, RetractPin=LOW");

  // Set movement tracking
  isActuatorMoving = true;
  targetMovement = "extend";
  actuatorMovementStart = millis();

  Serial.print("[EXTEND] ✓ Actuator extending started (60 seconds) - Source: ");
  Serial.println(source);
  Serial.print("[EXTEND] Movement start time: ");
  Serial.println(actuatorMovementStart);
}

// MODIFIED: Internal retract function with source parameter
void retractActuatorInternal(const char* source) {
  Serial.println("[RETRACT] retractActuatorInternal() executing...");
  Serial.print("[RETRACT] Source: ");
  Serial.println(source);
  
  // Check if already moving
  if (isActuatorMoving) {
    Serial.println("[RETRACT] BLOCKED: Actuator is currently moving. Please wait.");
    return;
  }

  // Check if cooldown is still active
  if (!isCooldownDone()) {
    unsigned long remainingTime = ACTUATOR_COOLDOWN_MS - (millis() - actuatorCooldownStart);
    Serial.print("[RETRACT] BLOCKED: Actuator is still in cooldown. Remaining: ");
    Serial.print(remainingTime / 1000);
    Serial.println(" seconds");
    return;
  }

  if (!isActuatorExtended && !isActuatorMoving) {
    Serial.println("[RETRACT] BLOCKED: Actuator is already retracted");
    return;
  }

  Serial.println("[RETRACT] Calling actuatorSafeTurnOff()...");
  actuatorSafeTurnOff();

  // Update Firestore state to "moving_retract" with source
  Serial.println("[RETRACT] Updating Firestore state to 'moving_retract'...");
  updateActuatorState("moving_retract", source);

  // Start physical movement
  Serial.println("[RETRACT] Starting physical movement...");
  digitalWrite(actuatorExtendPin, LOW);    // Relay1: COM→NC (GND)
  digitalWrite(actuatorRetractPin, HIGH);  // Relay2: COM→NO (12V+)
  Serial.println("[RETRACT] Relay pins set: ExtendPin=LOW, RetractPin=HIGH");

  // Set movement tracking
  isActuatorMoving = true;
  targetMovement = "retract";
  actuatorMovementStart = millis();

  Serial.print("[RETRACT] ✓ Actuator retracting started (60 seconds) - Source: ");
  Serial.println(source);
  Serial.print("[RETRACT] Movement start time: ");
  Serial.println(actuatorMovementStart);
}

void actuatorSafeTurnOff() {
  Serial.println("[SAFETY] actuatorSafeTurnOff() called");
  digitalWrite(actuatorExtendPin, LOW);
  digitalWrite(actuatorRetractPin, LOW);
  Serial.println("[SAFETY] Both relay pins set to LOW");
  delay(500); // Short delay for safety
  Serial.println("[SAFETY] ✓ Actuator safe turn-off activated");
}

void startActuatorCooldown() {
  actuatorCooldownStart = millis();
  cooldownActive = true;
  Serial.println("\n[COOLDOWN] Actuator cooldown started");
  Serial.print("[COOLDOWN] Duration: ");
  Serial.print(ACTUATOR_COOLDOWN_MS / 1000);
  Serial.println(" seconds");
  Serial.print("[COOLDOWN] Cooldown will end at millis: ");
  Serial.println(actuatorCooldownStart + ACTUATOR_COOLDOWN_MS);
}

bool isCooldownDone() {
  if (!cooldownActive) {
    return true;
  }

  if (millis() - actuatorCooldownStart >= ACTUATOR_COOLDOWN_MS) {
    cooldownActive = false;
    Serial.println("[COOLDOWN] ✓ Actuator cooldown completed");
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
