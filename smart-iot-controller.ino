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
#define DHTTYPE DHT11          // DHT11
#define RAIN_AO 35             // Rain sensor analog output
#define RAIN_DO 22             // Rain sensor digital output (LOW = rain detected)
#define LIGHT_AO 33
#define LIGHT_DO 16

DHT dht(DHTPIN, DHTTYPE);

// ===== Relay Logic (ACTIVE-HIGH) =====
// HIGH = relay ON  (coil energized)
// LOW  = relay OFF (coil de-energized)
//
// Linear actuator wiring:
//   EXTEND  → extendPin HIGH, retractPin LOW   (red wire gets 12V+)
//   RETRACT → extendPin LOW,  retractPin HIGH  (black wire gets 12V+)
//   STOP    → both LOW
#define RELAY_ON  HIGH
#define RELAY_OFF LOW

// ===== Rain thresholds (analog — lower = more rain) =====
// AO reads 0–4095 on ESP32 (12-bit ADC)
// Dry:    ~3000–4095
// Light:  ~2000–2999  → retract rack
// Heavy:  ~0–999      → extend rack BACK IN (protect from flooding/wind)
// DO pin: LOW = rain detected, HIGH = dry
#define RAIN_LIGHT_THRESHOLD  2000   // below this = rain → retract
#define RAIN_HEAVY_THRESHOLD  800    // below this = heavy rain → extend in

// ===== Timing =====
unsigned long lastUpdate = 0;
const unsigned long interval = 10000;            // Sensor push every 10s
unsigned long lastActuatorPoll = 0;
const unsigned long actuatorPollInterval = 5000; // Poll Firestore every 5s

// ===== Device Vars =====
const int actuatorExtendPin  = 25; // Relay1 — red wire
const int actuatorRetractPin = 26; // Relay2 — black wire
const int fansPin            = 13; // Relay3

bool isFansOpen = false;
bool isActuatorExtended = false;

// Actuator movement timing
unsigned long actuatorMovementStart = 0;
bool isActuatorMoving = false;
String targetMovement = "";
const unsigned long ACTUATOR_MOVEMENT_TIME = 60000; // 60 seconds
String lastKnownTarget = "";

// Cooldown after movement
unsigned long actuatorCooldownStart = 0;
bool cooldownActive = false;
const unsigned long ACTUATOR_COOLDOWN_MS = 20000; // 20 seconds

// Rain state machine
// Tracks which rain state we're currently handling to avoid repeated triggers
enum RainState { RAIN_NONE, RAIN_LIGHT, RAIN_HEAVY };
RainState currentRainState = RAIN_NONE;

bool rainNotificationSent = false;
bool heavyRainNotificationSent = false;

// Last sensor readings (used in notifications)
float lastTemp = 0;
float lastHum = 0;
int lastRainAO = 0;
int lastLightAO = 0;
bool lastRainDO = false; // true = dry, false = rain detected

// ===== HTTP Helpers =====
void httpPreDelay()  { delay(150); }
void httpPostDelay() { delay(200); }

// ===== Safety Net =====
// Always call before any movement direction change.
void motorSafeStop() {
  digitalWrite(actuatorExtendPin,  RELAY_OFF);
  digitalWrite(actuatorRetractPin, RELAY_OFF);
  delay(500);
  Serial.println("[SAFETY] Both pins OFF — motor stopped");
}

// ===== Setup =====
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);

  pinMode(actuatorExtendPin,  OUTPUT);
  pinMode(actuatorRetractPin, OUTPUT);
  pinMode(fansPin,            OUTPUT);

  digitalWrite(actuatorExtendPin,  RELAY_OFF);
  digitalWrite(actuatorRetractPin, RELAY_OFF);
  digitalWrite(fansPin,            RELAY_OFF);

  isActuatorExtended = false;
  isFansOpen = false;

  pinMode(RAIN_DO,  INPUT);
  pinMode(LIGHT_DO, INPUT);

  delay(1000);
  dht.begin();

  Serial.println("Connecting to WiFi...");
  WiFi.setTxPower(WIFI_POWER_15dBm);
  WiFi.begin(ssid, password);

  int wifi_retry = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_retry < 40) {
    delay(500);
    Serial.print(".");
    wifi_retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP: ");   Serial.println(WiFi.localIP());
    Serial.print("RSSI: "); Serial.println(WiFi.RSSI());
  } else {
    Serial.println("\nWiFi failed! Restarting...");
    delay(3000);
    ESP.restart();
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Waiting for NTP sync");
  delay(2000);
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    Serial.println("\nTime synced");
  } else {
    Serial.println("\nNTP sync failed");
  }

  Serial.println("ESP32 ready.");
}

// ===== Sensor Reads =====
float readTemperature() { float t = dht.readTemperature(); return isnan(t) ? -1.0 : t; }
float readHumidity()    { float h = dht.readHumidity();    return isnan(h) ? -1.0 : h; }
int   readRainAO()      { return analogRead(RAIN_AO); }
int   readLightAO()     { return analogRead(LIGHT_AO); }
bool  readRainDO()      { return digitalRead(RAIN_DO) == LOW; } // LOW = rain on sensor

// Classify rain level from AO value
RainState classifyRain(int rainAO, bool rainDO) {
  // Digital output confirms rain is present (belt and suspenders)
  if (!rainDO && rainAO >= RAIN_LIGHT_THRESHOLD) {
    return RAIN_NONE;   // sensor says dry
  }
  if (rainAO < RAIN_HEAVY_THRESHOLD) {
    return RAIN_HEAVY;  // very low AO = lots of water = heavy rain
  }
  if (rainAO < RAIN_LIGHT_THRESHOLD || rainDO) {
    return RAIN_LIGHT;  // moderate AO or DO triggered = light rain
  }
  return RAIN_NONE;
}

// ===== Firestore: Push Sensor Data =====
void sendToFirestore(float temp, float hum, int rainAO, int lightAO) {
  if (WiFi.status() != WL_CONNECTED) return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) { Serial.println("NTP fail"); return; }
  char timestamp[30];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

  StaticJsonDocument<512> doc;
  JsonObject fields = doc.createNestedObject("fields");
  fields["temperature"]["doubleValue"]  = temp;
  fields["humidity"]["doubleValue"]     = hum;
  fields["rainAO"]["integerValue"]      = rainAO;
  fields["light"]["integerValue"]       = lightAO;
  fields["updatedAt"]["timestampValue"] = timestamp;

  String payload;
  serializeJson(doc, payload);

  String url = String(FIRESTORE_URL)
    + "?updateMask.fieldPaths=temperature"
    + "&updateMask.fieldPaths=humidity"
    + "&updateMask.fieldPaths=rainAO"
    + "&updateMask.fieldPaths=light"
    + "&updateMask.fieldPaths=updatedAt";

  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;

  httpPreDelay();
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.PATCH(payload);
  http.end();
  httpPostDelay();

  Serial.println(httpCode == 200 ? "✓ Sensor data sent" : "✗ Sensor failed: " + String(httpCode));
}

// ===== Firestore: Update Actuator State =====
void updateActuatorState(const char* state, const char* source) {
  if (WiFi.status() != WL_CONNECTED) return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) { Serial.println("NTP fail"); return; }
  char timestamp[30];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

  StaticJsonDocument<512> doc;
  JsonObject fields = doc.createNestedObject("fields");
  JsonObject actuator = fields.createNestedObject("actuator")
                              .createNestedObject("mapValue")
                              .createNestedObject("fields");
  actuator["state"]["stringValue"]        = state;
  actuator["updatedAt"]["timestampValue"] = timestamp;
  actuator["source"]["stringValue"]       = source;

  String payload;
  serializeJson(doc, payload);

  String url = ACTUATOR_URL
    + "?updateMask.fieldPaths=actuator.state"
    + "&updateMask.fieldPaths=actuator.updatedAt"
    + "&updateMask.fieldPaths=actuator.source";

  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;

  httpPreDelay();
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.PATCH(payload);
  http.end();
  httpPostDelay();

  Serial.print("[STATE] "); Serial.print(state);
  Serial.println(httpCode == 200 ? " ✓" : " ✗ code=" + String(httpCode));
}

// ===== Firestore: Send Notification (full schema) =====
void sendNotification(
  const char* title,
  const char* body,
  const char* type,       // "alert", "info", "warning", "success"
  const char* category,   // "weather", "system", "device", "manual"
  const char* trigger,    // "rain_sensor", "user", etc.
  const char* action,     // "actuator_retracted", "actuator_extended", etc.
  const char* priority,   // "low", "medium", "high", "critical"
  float temp, float hum, int rainAO, int lightAO
) {
  if (WiFi.status() != WL_CONNECTED) return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;
  char timestamp[30];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

  // expiresAt = 30 days from now (optional auto-delete)
  time_t now;
  time(&now);
  now += 30 * 24 * 3600;
  struct tm* expireInfo = gmtime(&now);
  char expireTimestamp[30];
  strftime(expireTimestamp, sizeof(expireTimestamp), "%Y-%m-%dT%H:%M:%SZ", expireInfo);

  StaticJsonDocument<1024> doc;
  JsonObject fields = doc.createNestedObject("fields");

  // Core Fields
  fields["title"]["stringValue"]         = title;
  fields["body"]["stringValue"]          = body;
  fields["time"]["timestampValue"]       = timestamp;
  fields["isRead"]["booleanValue"]       = false;

  // Categorization
  fields["type"]["stringValue"]          = type;
  fields["category"]["stringValue"]      = category;

  // Source Context
  fields["deviceId"]["stringValue"]      = DEVICE_ID;
  fields["trigger"]["stringValue"]       = trigger;

  // Action/State Data
  fields["action"]["stringValue"]        = action;

  // Sensor Data map
  JsonObject sd = fields["sensorData"].createNestedObject("mapValue").createNestedObject("fields");
  sd["temperature"]["doubleValue"] = temp;
  sd["humidity"]["doubleValue"]    = hum;
  sd["rainAO"]["integerValue"]     = rainAO;
  sd["lightAO"]["integerValue"]    = lightAO;

  // Metadata
  fields["priority"]["stringValue"]      = priority;
  fields["acknowledged"]["booleanValue"] = false;
  fields["createdAt"]["timestampValue"]  = timestamp;
  fields["expiresAt"]["timestampValue"]  = expireTimestamp;

  String payload;
  serializeJson(doc, payload);

  String url = "https://firestore.googleapis.com/v1/projects/smart-drying-iot/databases/(default)/documents/notifications";

  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;

  httpPreDelay();
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.POST(payload);
  http.end();
  httpPostDelay();

  Serial.println(httpCode == 200 ? "[NOTIFY] ✓ Sent" : "[NOTIFY] ✗ Failed: " + String(httpCode));
}

// ===== Firestore: Poll Actuator Target =====
void pollActuatorTarget() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;

  httpPreDelay();
  http.begin(client, ACTUATOR_URL);
  int httpCode = http.GET();

  if (httpCode != 200) {
    Serial.print("[POLL] Failed: "); Serial.println(httpCode);
    http.end();
    httpPostDelay();
    return;
  }

  String response = http.getString();
  http.end();
  httpPostDelay();

  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, response)) { Serial.println("[POLL] JSON error"); return; }

  JsonVariant targetVar = doc["fields"]["actuator"]["mapValue"]["fields"]["target"]["stringValue"];
  if (targetVar.isNull()) return;

  String newTarget = targetVar.as<String>();
  if (newTarget.length() == 0) return;

  if (lastKnownTarget.length() == 0) {
    lastKnownTarget = newTarget;
    Serial.print("[POLL] Initial target: "); Serial.println(newTarget);
    return;
  }

  if (newTarget == lastKnownTarget) return;

  Serial.print("[POLL] Target: "); Serial.print(lastKnownTarget);
  Serial.print(" → "); Serial.println(newTarget);
  lastKnownTarget = newTarget;

  if      (newTarget == "extend")  extendActuatorFromUser();
  else if (newTarget == "retract") retractActuatorFromUser();
  else { Serial.print("[POLL] Unknown: "); Serial.println(newTarget); }
}

// ===== Rain State Handler =====
// Called every loop. Handles transitions between NONE, LIGHT, HEAVY.
void handleRainState(RainState newState) {
  if (newState == currentRainState) return; // no change

  Serial.print("[RAIN] State: ");
  Serial.print(currentRainState);
  Serial.print(" → ");
  Serial.println(newState);

  currentRainState = newState;

  if (newState == RAIN_NONE) {
    // Rain cleared — reset flags
    rainNotificationSent = false;
    heavyRainNotificationSent = false;
    Serial.println("[RAIN] Cleared — notifications reset");

  } else if (newState == RAIN_LIGHT) {
    // Light rain → RETRACT rack to protect clothes
    Serial.println("[RAIN] Light rain → retracting rack");
    retractActuatorFromRain();

    if (!rainNotificationSent) {
      sendNotification(
        "Rain Detected",
        "Light rain detected. Drying rack is being retracted to protect your clothes.",
        "alert",           // type
        "weather",         // category
        "rain_sensor",     // trigger
        "actuator_retract",// action
        "high",            // priority
        lastTemp, lastHum, lastRainAO, lastLightAO
      );
      rainNotificationSent = true;
    }

  } else if (newState == RAIN_HEAVY) {
    // Heavy rain → EXTEND rack back to protect it from wind/flooding
    Serial.println("[RAIN] Heavy rain → extending rack for protection");
    extendActuatorFromRain();

    if (!heavyRainNotificationSent) {
      sendNotification(
        "Heavy Rain Alert!",
        "Heavy rain detected. Rack is being extended for structural protection.",
        "warning",              // type
        "weather",              // category
        "rain_sensor",          // trigger
        "actuator_extend",      // action
        "critical",             // priority
        lastTemp, lastHum, lastRainAO, lastLightAO
      );
      heavyRainNotificationSent = true;
    }
  }
}

// ===== Main Loop =====
void loop() {
  unsigned long now = millis();

  // Read all sensors
  float temp  = readTemperature();
  float hum   = readHumidity();
  int rainAO  = readRainAO();
  int lightAO = readLightAO();
  bool rainDO = readRainDO(); // true = rain on sensor surface

  lastTemp    = temp;
  lastHum     = hum;
  lastRainAO  = rainAO;
  lastLightAO = lightAO;
  lastRainDO  = rainDO;

  // Classify and handle rain state
  RainState detectedState = classifyRain(rainAO, rainDO);
  handleRainState(detectedState);

  // --- Actuator movement: auto-stop after 60 seconds ---
  if (isActuatorMoving) {
    unsigned long elapsed = now - actuatorMovementStart;

    if (elapsed >= ACTUATOR_MOVEMENT_TIME) {
      Serial.println("[MOVEMENT] 60s elapsed — stopping motor");
      isActuatorMoving = false;
      motorSafeStop();

      const char* source = (currentRainState != RAIN_NONE) ? "rain_sensor" : "user";

      if (targetMovement == "extend") {
        updateActuatorState("extended", source);
        isActuatorExtended = true;
        Serial.println("[MOVEMENT] ✓ Extended");
      } else if (targetMovement == "retract") {
        updateActuatorState("retracted", source);
        isActuatorExtended = false;
        Serial.println("[MOVEMENT] ✓ Retracted");
      }

      targetMovement = "";
      startActuatorCooldown();

    } else if (elapsed % 10000 < 200) {
      Serial.print("[MOVEMENT] Moving... ");
      Serial.print(elapsed / 1000);
      Serial.println("/60s");
    }
  }

  // --- Poll Firestore for user commands (every 5s) ---
  if (now - lastActuatorPoll >= actuatorPollInterval) {
    lastActuatorPoll = now;
    pollActuatorTarget();
  }

  // --- Push sensor data (every 10s) ---
  if (now - lastUpdate >= interval) {
    lastUpdate = now;

    Serial.println("\n=== Sensor Readings ===");
    Serial.print("Temp: ");     Serial.print(temp);    Serial.println(" °C");
    Serial.print("Hum:  ");    Serial.print(hum);     Serial.println(" %");
    Serial.print("Rain AO: "); Serial.print(rainAO);
    Serial.print("  DO: ");    Serial.println(rainDO ? "WET" : "DRY");
    Serial.print("Rain state: ");
    if (detectedState == RAIN_NONE)   Serial.println("NONE");
    if (detectedState == RAIN_LIGHT)  Serial.println("LIGHT");
    if (detectedState == RAIN_HEAVY)  Serial.println("HEAVY");
    Serial.print("Light AO: "); Serial.println(lightAO);
    Serial.println("=======================");

    sendToFirestore(temp, hum, rainAO, lightAO);
  }

  delay(100);
}

// ===== Actuator Wrappers =====
void extendActuatorFromUser()  { extendActuatorInternal("user"); }
void extendActuatorFromRain()  { extendActuatorInternal("rain_sensor"); }
void retractActuatorFromRain() { retractActuatorInternal("rain_sensor"); }
void retractActuatorFromUser() { retractActuatorInternal("user"); }

// ===== EXTEND: extendPin HIGH, retractPin LOW =====
void extendActuatorInternal(const char* source) {
  if (isActuatorMoving)   { Serial.println("[EXTEND] BLOCKED: already moving");   return; }
  if (!isCooldownDone())  { Serial.println("[EXTEND] BLOCKED: cooldown active");  return; }
  if (isActuatorExtended) { Serial.println("[EXTEND] BLOCKED: already extended"); return; }

  if (isFansOpen) closeFans();

  motorSafeStop(); // Safety net before engaging

  updateActuatorState("moving_extend", source);

  digitalWrite(actuatorExtendPin,  RELAY_ON);   // HIGH — red wire power
  digitalWrite(actuatorRetractPin, RELAY_OFF);  // LOW  — black wire off

  isActuatorMoving = true;
  targetMovement = "extend";
  actuatorMovementStart = millis();

  Serial.print("[EXTEND] ✓ Motor running (60s) — source: "); Serial.println(source);
}

// ===== RETRACT: extendPin LOW, retractPin HIGH =====
void retractActuatorInternal(const char* source) {
  if (isActuatorMoving)                         { Serial.println("[RETRACT] BLOCKED: already moving");    return; }
  if (!isCooldownDone())                        { Serial.println("[RETRACT] BLOCKED: cooldown active");   return; }
  if (!isActuatorExtended && !isActuatorMoving) { Serial.println("[RETRACT] BLOCKED: already retracted"); return; }

  motorSafeStop(); // Safety net before engaging

  updateActuatorState("moving_retract", source);

  digitalWrite(actuatorExtendPin,  RELAY_OFF);  // LOW  — red wire off
  digitalWrite(actuatorRetractPin, RELAY_ON);   // HIGH — black wire power

  isActuatorMoving = true;
  targetMovement = "retract";
  actuatorMovementStart = millis();

  Serial.print("[RETRACT] ✓ Motor running (60s) — source: "); Serial.println(source);
}

// ===== Cooldown =====
void startActuatorCooldown() {
  actuatorCooldownStart = millis();
  cooldownActive = true;
  Serial.print("[COOLDOWN] Started — ");
  Serial.print(ACTUATOR_COOLDOWN_MS / 1000);
  Serial.println("s");
}

bool isCooldownDone() {
  if (!cooldownActive) return true;
  if (millis() - actuatorCooldownStart >= ACTUATOR_COOLDOWN_MS) {
    cooldownActive = false;
    Serial.println("[COOLDOWN] ✓ Done");
    return true;
  }
  return false;
}

// ===== Fan Control =====
void openFans() {
  if (isFansOpen) return;
  if (isActuatorExtended || isActuatorMoving) {
    Serial.println("Fans blocked: actuator active");
    return;
  }
  digitalWrite(fansPin, RELAY_ON);
  isFansOpen = true;
  Serial.println("Fans opened");
}

void closeFans() {
  if (!isFansOpen) return;
  digitalWrite(fansPin, RELAY_OFF);
  isFansOpen = false;
  Serial.println("Fans closed");
}
