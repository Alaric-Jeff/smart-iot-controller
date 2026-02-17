#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include "DHT.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ============================================================
// CREDENTIALS & CONFIG
// ============================================================
const char* ssid = "vivo V15";
const char* password = "pogiako123";

#define API_KEY "AIzaSyCkPtysjodxh356vyuQSaAhg59xjeHjMVU"
#define DATABASE_URL "https://smart-drying-iot-default-rtdb.asia-southeast1.firebasedatabase.app/"

const String DEVICE_ID = "00:1A:2B:3C:4D:5E";
const String ACTUATOR_PATH     = "/devices/" + DEVICE_ID + "/actuator";
const String SENSOR_PATH       = "/devices/" + DEVICE_ID + "/sensors";
const String NOTIFICATION_PATH = "/devices/" + DEVICE_ID + "/notifications";
const String FAN_PATH          = "/devices/" + DEVICE_ID + "/fans";

// ============================================================
// PIN DEFINITIONS
// ============================================================
#define DHTPIN    23
#define DHTTYPE   DHT11
#define RAIN_AO   35
#define RAIN_DO   22
#define LIGHT_AO  33
#define LIGHT_DO  16

const int RPWM = 25;
const int LPWM = 26;
const int R_EN = 27;
const int L_EN = 14;

// --- DUAL FAN PWM pins — each drives one F5305S MOSFET gate ---
// Fan 1: GPIO 32  |  Fan 2: GPIO 4
// Both receive identical duty cycle — 1 command = both fans obey
#define FAN1_PIN  32
#define FAN2_PIN   4

// --- LEDC PWM config for F5305S MOSFETs ---
// ESP32 core v3.x API change:
//   OLD (v2): ledcSetup(ch, freq, res) + ledcAttachPin(pin, ch) + ledcWrite(ch, duty)
//   NEW (v3): ledcAttach(pin, freq, res) + ledcWrite(pin, duty) + ledcDetach(pin)
//   Channels are now assigned automatically — no channel numbers needed.
//
// F5305S: Input signal 3V–20V, 5mA — ESP32 3.3V logic is within spec
// 25 kHz is above audible range and well-suited for power MOSFET switching
// 8-bit resolution gives duty range 0–255
#define FAN_LEDC_FREQ         25000 // 25 kHz PWM frequency
#define FAN_LEDC_RESOLUTION   8     // 8-bit = 0–255 duty range

#define RAIN_LIGHT_THRESHOLD  2000
#define RAIN_HEAVY_THRESHOLD  800

// ============================================================
// GLOBAL VARIABLES
// ============================================================
DHT dht(DHTPIN, DHTTYPE);

FirebaseData fbdo;
FirebaseData stream;
FirebaseData fanStream;
FirebaseAuth auth;
FirebaseConfig config;

String currentPhysicalState = "retracted";
unsigned long lastSensorUpdate = 0;
const unsigned long SENSOR_INTERVAL = 10000;

// Sensor data cache for notifications
float lastTemp     = 0;
float lastHumidity = 0;
int   lastRainVal  = 0;
int   lastLightVal = 0;

enum RainState { RAIN_NONE, RAIN_LIGHT, RAIN_HEAVY };
RainState currentRainState = RAIN_NONE;

// Fan state variables
String currentFanState = "off";  // "on" or "off"
String currentFanSpeed = "low";  // "low", "mid", "high"

// FIX #2: Boot stream skip flag.
// initFanNodeOnBoot() writes to the fans/ node BEFORE the stream is
// registered, but RTDB streams always deliver the current node value
// as the very first event upon subscription. This flag causes
// fanStreamCallback to discard that first synthetic event so it is
// never mistaken for a real user command (which would call
// stopBothFans() unnecessarily and could override a running session
// if the ESP32 reboots mid-use).
bool _fanStreamBootSkipDone = false;

// ============================================================
// FORWARD DECLARATIONS
// ============================================================
void extendActuatorInternal(const char* source);
void retractActuatorInternal(const char* source);
void updateCloudState(String state, String source);
void motorStop();
void streamCallback(FirebaseStream data);
void streamTimeoutCallback(bool timeout);
void rejectCommandAndRevert(const char* reason);
void rejectFanCommandAndRevert(const char* reason);
void createNotification(String title, String body, String type, String category, String trigger, String action, String priority);
void fanStreamCallback(FirebaseStream data);
void fanStreamTimeoutCallback(bool timeout);
void applyFanSpeed(String speed);
void stopBothFans();
void updateFanCloudState(String state, String speed);
void initFanNodeOnBoot();

// ============================================================
// NOTIFICATION HELPER
// ============================================================
void createNotification(String title, String body, String type, String category, String trigger, String action, String priority) {
  if (!Firebase.ready()) return;

  String notifId   = "notif_" + String(millis());
  String notifPath = NOTIFICATION_PATH + "/" + notifId;

  FirebaseJson notifJson;
  notifJson.set("title",    title);
  notifJson.set("body",     body);
  notifJson.set("time",     (int)millis());
  notifJson.set("isRead",   false);
  notifJson.set("type",     type);
  notifJson.set("category", category);
  notifJson.set("deviceId", DEVICE_ID);
  notifJson.set("trigger",  trigger);
  notifJson.set("action",   action);

  FirebaseJson sensorData;
  sensorData.set("temperature", lastTemp);
  sensorData.set("humidity",    lastHumidity);
  sensorData.set("rainAO",      lastRainVal);
  sensorData.set("lightAO",     lastLightVal);
  notifJson.set("sensorData", sensorData);

  notifJson.set("priority",     priority);
  notifJson.set("acknowledged", false);
  notifJson.set("createdAt",    (int)millis());

  Firebase.RTDB.setJSONAsync(&fbdo, notifPath.c_str(), &notifJson);
  Serial.printf("[NOTIFICATION] Created: %s - %s\n", title.c_str(), body.c_str());
}

// ============================================================
// ACTUATOR STREAM CALLBACK
// ============================================================
void streamCallback(FirebaseStream data) {
  Serial.println("[STREAM] Data received");
  Serial.println("Path: " + data.dataPath());
  Serial.println("Type: " + data.dataType());

  String targetState = "";

  if (data.dataType() == "json") {
    FirebaseJson &json = data.jsonObject();
    FirebaseJsonData result;
    if (json.get(result, "target")) {
      targetState = result.stringValue;
    }
  } else if (data.dataType() == "string") {
    targetState = data.stringData();
  }

  targetState.replace("\"", "");
  targetState.trim();

  if (targetState.length() > 0) {
    Serial.print("[STREAM] Target: ");
    Serial.println(targetState);

    // --- SAFETY GUARD 1: Block extend during heavy rain ---
    if (targetState == "extended" && currentRainState == RAIN_HEAVY) {
      Serial.println("[SAFETY] REJECTED: Cannot extend during heavy rain!");
      rejectCommandAndRevert("heavy_rain_detected");
      createNotification(
        "Command Blocked",
        "Cannot extend actuator during heavy rain.",
        "warning", "system", "user_command_rejected", "command_blocked", "high"
      );
      return;
    }

    // --- SAFETY GUARD 2: Block extend while fans are ON ---
    // Actuator CANNOT extend if fans are currently running
    if (targetState == "extended" && currentFanState == "on") {
      Serial.println("[SAFETY] REJECTED: Cannot extend actuator while fans are ON!");
      rejectCommandAndRevert("fans_are_on");
      createNotification(
        "Command Blocked",
        "Turn off the drying fans before extending the actuator.",
        "warning", "system", "user_command_rejected", "command_blocked", "high"
      );
      return;
    }

    if (targetState == "extended" && currentPhysicalState != "extended") {
      extendActuatorInternal("app_command");
    }
    else if (targetState == "retracted" && currentPhysicalState != "retracted") {
      retractActuatorInternal("app_command");
    }
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("[STREAM] Timeout, resuming...");
}

// ============================================================
// FAN STREAM CALLBACK
// ============================================================
void fanStreamCallback(FirebaseStream data) {
  // FIX #2: Discard the first event fired on stream subscription.
  // RTDB always delivers the current node value immediately when a
  // stream is opened. Since initFanNodeOnBoot() already set the node
  // to {state:"off", target:"off"} moments before the stream was
  // registered, this first event is a synthetic echo of that boot
  // write — not a real user command. Skipping it prevents:
  //   (a) an unnecessary stopBothFans() call on every reboot, and
  //   (b) overwriting a fan session that was active before a reboot.
  if (!_fanStreamBootSkipDone) {
    _fanStreamBootSkipDone = true;
    Serial.println("[FAN STREAM] Boot echo discarded — ignoring first event");
    return;
  }

  Serial.println("[FAN STREAM] Data received");
  Serial.println("Path: " + data.dataPath());
  Serial.println("Type: " + data.dataType());

  String target = "";
  String speed  = "";

  if (data.dataType() == "json") {
    FirebaseJson &json = data.jsonObject();
    FirebaseJsonData result;

    if (json.get(result, "target")) {
      target = result.stringValue;
      target.replace("\"", "");
      target.trim();
    }
    if (json.get(result, "speed")) {
      speed = result.stringValue;
      speed.replace("\"", "");
      speed.trim();
    }
  } else if (data.dataType() == "string") {
    if (data.dataPath() == "/target") {
      target = data.stringData();
      target.replace("\"", "");
      target.trim();
      speed = currentFanSpeed; // keep current speed
    } else if (data.dataPath() == "/speed") {
      // Speed changed while fan is on — apply immediately
      speed = data.stringData();
      speed.replace("\"", "");
      speed.trim();
      target = currentFanState; // keep current on/off state
    }
  }

  Serial.printf("[FAN STREAM] target=%s speed=%s\n", target.c_str(), speed.c_str());

  // Validate speed, fallback to current if invalid
  if (speed != "low" && speed != "mid" && speed != "high") {
    speed = currentFanSpeed;
  }

  if (target == "on") {
    // --- SAFETY GUARD 3: Block fans from turning ON while actuator is extended ---
    // Fans can ONLY operate when actuator is fully retracted
    if (currentPhysicalState == "extended") {
      Serial.println("[SAFETY] REJECTED: Cannot turn fans ON while actuator is extended!");
      rejectFanCommandAndRevert("actuator_is_extended");
      createNotification(
        "Command Blocked",
        "Retract the actuator before turning on the drying fans.",
        "warning", "system", "user_command_rejected", "command_blocked", "high"
      );
      return;
    }

    currentFanSpeed = speed;
    currentFanState = "on";
    applyFanSpeed(currentFanSpeed);
    updateFanCloudState("on", currentFanSpeed);
    Serial.printf("[FAN] Both fans ON at speed: %s\n", currentFanSpeed.c_str());
  }
  else if (target == "off") {
    currentFanState = "off";
    stopBothFans();
    updateFanCloudState("off", currentFanSpeed); // keep last speed in RTDB
    Serial.println("[FAN] Both fans OFF");
  }
}

void fanStreamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("[FAN STREAM] Timeout, resuming...");
}

// ============================================================
// FAN HELPERS — Dual F5305S MOSFETs via ESP32 LEDC PWM
//
// ESP32 core v3.x API — pin-based, no channel numbers:
//   ledcAttach(pin, freq, resolution)  → configure & attach in one call
//   ledcWrite(pin, duty)               → write duty by pin, not channel
//   ledcDetach(pin)                    → release pin if needed
//
// F5305S specs:
//   Input voltage:  3V–20V  (ESP32 3.3V GPIO is within spec)
//   Input current:  ~5mA    (ESP32 GPIO can source this directly)
//   Output voltage: 5V–36V  (12V PC fans = well within range)
//   Output current: 5A cont / 20A max (PC fans ~0.5–2A each = safe)
//
// LEDC duty values (8-bit, 0–255):
//   LOW  =  80  (~31% duty) — quiet, gentle airflow
//   MID  = 160  (~63% duty) — medium airflow
//   HIGH = 255  (100% duty) — full speed
// ============================================================
void applyFanSpeed(String speed) {
  uint8_t duty = 80; // default = low
  if      (speed == "low")  duty = 80;
  else if (speed == "mid")  duty = 160;
  else if (speed == "high") duty = 255;

  // v3 API: write duty by pin directly — no channel numbers
  ledcWrite(FAN1_PIN, duty);
  ledcWrite(FAN2_PIN, duty);

  Serial.printf("[FAN PWM] speed=%s duty=%d → Fan1(GPIO%d) Fan2(GPIO%d)\n",
    speed.c_str(), duty, FAN1_PIN, FAN2_PIN
  );
}

void stopBothFans() {
  // v3 API: write duty by pin directly — no channel numbers
  ledcWrite(FAN1_PIN, 0);
  ledcWrite(FAN2_PIN, 0);

  Serial.printf("[FAN PWM] Both fans stopped → Fan1(GPIO%d) Fan2(GPIO%d)\n",
    FAN1_PIN, FAN2_PIN
  );
}

void updateFanCloudState(String state, String speed) {
  if (!Firebase.ready()) return;

  FirebaseJson json;
  json.set("state",         state);
  json.set("speed",         speed);
  json.set("target",        state);  // Sync target with confirmed state
  json.set("lastCommandAt", (int)millis());

  Firebase.RTDB.updateNodeAsync(&fbdo, FAN_PATH.c_str(), &json);
  Serial.printf("[FAN CLOUD] state=%s speed=%s\n", state.c_str(), speed.c_str());
}

// ============================================================
// FAN REJECT HELPER
// Mirrors rejectCommandAndRevert() but for the fans/ RTDB node.
// Reverts fan target back to "off" when a safety interlock fires.
// Writes state:"off" so Flutter fan listener clears
// _isFanCommandProcessing immediately.
// ============================================================
void rejectFanCommandAndRevert(const char* reason) {
  if (!Firebase.ready()) return;

  FirebaseJson json;
  json.set("target",          "off");
  json.set("state",           "off");   // Flutter fan listener checks state field
  json.set("commandRejected", true);
  json.set("rejectionReason", reason);
  json.set("lastCommandAt",   (int)millis());

  Firebase.RTDB.updateNodeAsync(&fbdo, FAN_PATH.c_str(), &json);
  Serial.printf("[SAFETY] Fan command rejected: %s — target+state reverted to 'off'\n", reason);
}

// ============================================================
// FAN NODE BOOT INITIALIZER
// Creates fans/ node in RTDB on boot so it appears in the tree
// immediately under devices/DEVICE_ID/ alongside actuator/,
// sensors/, and notifications/
// ============================================================
void initFanNodeOnBoot() {
  if (!Firebase.ready()) return;

  FirebaseJson json;
  json.set("state",         "off");
  json.set("speed",         "low");
  json.set("target",        "off");
  json.set("lastCommandAt", (int)millis());

  if (Firebase.RTDB.updateNodeAsync(&fbdo, FAN_PATH.c_str(), &json)) {
    Serial.println("[BOOT] fans/ node initialized in RTDB");
  } else {
    Serial.printf("[BOOT] fans/ node init failed: %s\n", fbdo.errorReason().c_str());
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  delay(1000);

  pinMode(RPWM, OUTPUT);
  pinMode(LPWM, OUTPUT);
  pinMode(R_EN, OUTPUT);
  pinMode(L_EN, OUTPUT);

  digitalWrite(R_EN, HIGH);
  digitalWrite(L_EN, HIGH);
  motorStop();

  // --- DUAL FAN: Configure LEDC via ESP32 core v3.x API ---
  // v3 replaces the 3-step v2 flow (ledcSetup + ledcAttachPin + ledcWrite)
  // with a single ledcAttach(pin, freq, resolution) call per pin.
  // Channels are assigned automatically by the driver — no channel
  // numbers are needed or used anywhere in v3.
  //
  // Fan 1 → GPIO 32 → MOSFET 1 → Fan 1
  // Fan 2 → GPIO 4  → MOSFET 2 → Fan 2
  ledcAttach(FAN1_PIN, FAN_LEDC_FREQ, FAN_LEDC_RESOLUTION);
  ledcWrite(FAN1_PIN, 0); // Fan 1 OFF on boot

  ledcAttach(FAN2_PIN, FAN_LEDC_FREQ, FAN_LEDC_RESOLUTION);
  ledcWrite(FAN2_PIN, 0); // Fan 2 OFF on boot

  pinMode(RAIN_DO, INPUT);
  pinMode(LIGHT_DO, INPUT);
  dht.begin();

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nConnected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  config.api_key               = API_KEY;
  config.database_url          = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;

  auth.user.email    = "";
  auth.user.password = "";

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.println("Signing in anonymously...");
  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Anonymous sign-in successful!");
  } else {
    Serial.printf("Sign-in failed: %s\n", config.signer.signupError.message.c_str());
  }

  Serial.println("Waiting for Firebase...");
  while (!Firebase.ready()) {
    Serial.print(".");
    delay(100);
  }
  Serial.println("\nFirebase ready!");

  // Initialize fans/ node so it appears in RTDB tree on boot.
  // NOTE: This write happens BEFORE the fan stream is registered below,
  // but RTDB streams echo the current node value on first subscription
  // regardless. _fanStreamBootSkipDone handles that echo in the callback.
  initFanNodeOnBoot();

  // Actuator stream
  if (!Firebase.RTDB.beginStream(&stream, ACTUATOR_PATH.c_str())) {
    Serial.println("Stream error: " + stream.errorReason());
  } else {
    Firebase.RTDB.setStreamCallback(&stream, streamCallback, streamTimeoutCallback);
    Serial.println("[RTDB] Actuator stream started on: " + ACTUATOR_PATH);
  }

  // Fan stream
  if (!Firebase.RTDB.beginStream(&fanStream, FAN_PATH.c_str())) {
    Serial.println("Fan stream error: " + fanStream.errorReason());
  } else {
    Firebase.RTDB.setStreamCallback(&fanStream, fanStreamCallback, fanStreamTimeoutCallback);
    Serial.println("[RTDB] Fan stream started on: " + FAN_PATH);
  }
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  unsigned long now = millis();

  if (now - lastSensorUpdate >= SENSOR_INTERVAL) {
    lastSensorUpdate = now;

    float t     = dht.readTemperature();
    float h     = dht.readHumidity();
    int   rain  = analogRead(RAIN_AO);
    int   light = analogRead(LIGHT_AO);

    if (isnan(t)) t = 0;
    if (isnan(h)) h = 0;

    lastTemp     = t;
    lastHumidity = h;
    lastRainVal  = rain;
    lastLightVal = light;

    Serial.printf("[SENSORS] T:%.1f H:%.1f Rain:%d Light:%d\n", t, h, rain, light);

    FirebaseJson json;
    json.set("temperature", t);
    json.set("humidity",    h);
    json.set("rainAO",      rain);
    json.set("light",       light);
    json.set("updatedAt",   (int)now);

    if (Firebase.ready()) {
      Firebase.RTDB.updateNodeAsync(&fbdo, SENSOR_PATH.c_str(), &json);
    }
  }

  // ============================================================
  // RAIN SENSOR LOGIC
  // ============================================================
  int  rainVal = analogRead(RAIN_AO);
  bool rainDig = digitalRead(RAIN_DO) == LOW;

  if (rainVal < RAIN_HEAVY_THRESHOLD) {
    if (currentRainState != RAIN_HEAVY) {
      currentRainState = RAIN_HEAVY;
      if (currentPhysicalState != "retracted") {
        Serial.println("[RAIN] HEAVY! Retracting...");
        retractActuatorInternal("rain_heavy");
        createNotification(
          "Heavy Rain Detected!",
          "Actuator retracted due to heavy rainfall.",
          "alert", "weather", "rain_sensor", "actuator_retracted", "high"
        );
      } else {
        Serial.println("[RAIN] HEAVY detected, but already retracted - no action needed");
      }
    }
  } else if (rainVal < RAIN_LIGHT_THRESHOLD || rainDig) {
    if (currentRainState != RAIN_LIGHT) {
      currentRainState = RAIN_LIGHT;
      if (currentPhysicalState != "retracted") {
        Serial.println("[RAIN] LIGHT! Retracting...");
        retractActuatorInternal("rain_light");
        createNotification(
          "Rain Detected",
          "Actuator retracted due to light rain.",
          "info", "weather", "rain_sensor", "actuator_retracted", "medium"
        );
      }
    }
  } else {
    currentRainState = RAIN_NONE;
  }

  delay(10);
}

// ============================================================
// ACTUATOR HELPERS
// ============================================================
void motorStop() {
  analogWrite(RPWM, 0);
  analogWrite(LPWM, 0);
}

void updateCloudState(String state, String source) {
  if (!Firebase.ready()) return;

  FirebaseJson json;
  json.set("state",         state);
  json.set("source",        source);
  json.set("target",        state);
  json.set("lastCommandAt", (int)millis());

  Firebase.RTDB.updateNodeAsync(&fbdo, ACTUATOR_PATH.c_str(), &json);
}

// ============================================================
// REJECT & REVERT — ACTUATOR
// FIX: Now writes "state" field in addition to "target" and
// "commandRejected". Flutter's actuator listener checks the
// "state" field to clear _isCommandProcessing. Without writing
// state here, Flutter would stay locked for the full 60s
// cooldown after a rejected command even though nothing moved.
// ============================================================
void rejectCommandAndRevert(const char* reason) {
  if (!Firebase.ready()) return;

  FirebaseJson json;
  json.set("target",          "retracted");
  json.set("state",           currentPhysicalState); // FIX: write state so Flutter listener fires
  json.set("commandRejected", true);
  json.set("rejectionReason", reason);
  json.set("lastCommandAt",   (int)millis());

  Firebase.RTDB.updateNodeAsync(&fbdo, ACTUATOR_PATH.c_str(), &json);
  Serial.printf("[SAFETY] Command rejected: %s - Target reverted to 'retracted'\n", reason);
}

void extendActuatorInternal(const char* source) {
  if (currentPhysicalState == "extended") return;

  motorStop();
  delay(100);

  analogWrite(RPWM, 255);
  analogWrite(LPWM, 0);

  currentPhysicalState = "extended";
  updateCloudState("extended", source);
  Serial.println("[ACTUATOR] Extended");
}

void retractActuatorInternal(const char* source) {
  if (currentPhysicalState == "retracted") return;

  motorStop();
  delay(100);

  analogWrite(RPWM, 0);
  analogWrite(LPWM, 255);

  currentPhysicalState = "retracted";
  updateCloudState("retracted", source);
  Serial.println("[ACTUATOR] Retracted");
}
