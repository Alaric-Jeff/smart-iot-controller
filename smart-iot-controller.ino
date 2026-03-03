#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include "DHT.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// --- BLE ---
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ============================================================
// CREDENTIALS & CONFIG
// ============================================================
const char* ssid     = "vivo V15";
const char* password = "pogiako123";

#define API_KEY       "AIzaSyCkPtysjodxh356vyuQSaAhg59xjeHjMVU"
#define DATABASE_URL  "https://smart-drying-iot-default-rtdb.asia-southeast1.firebasedatabase.app/"

const String DEVICE_ID         = "00:1A:2B:3C:4D:5E";
const String ACTUATOR_PATH     = "/devices/" + DEVICE_ID + "/actuator";
const String SENSOR_PATH       = "/devices/" + DEVICE_ID + "/sensors";
const String NOTIFICATION_PATH = "/devices/" + DEVICE_ID + "/notifications";
const String FAN_PATH          = "/devices/" + DEVICE_ID + "/fans";

// ============================================================
// BLE CONFIG
// ============================================================
#define BLE_DEVICE_NAME       "SmartRack-ESP32"
#define BLE_SERVICE_UUID      "12345678-1234-1234-1234-123456789abc"
#define BLE_STATUS_CHAR_UUID  "12345678-1234-1234-1234-123456789abd"
#define BLE_COMMAND_CHAR_UUID "12345678-1234-1234-1234-123456789abe"

BLEServer* pServer            = nullptr;
BLECharacteristic* pStatusChar        = nullptr;
BLECharacteristic* pCommandChar       = nullptr;
bool               bleClientConnected = false;
bool               bleMode            = false;

// ============================================================
// PIN DEFINITIONS
// ============================================================
#define DHTPIN    23
#define DHTTYPE   DHT22
#define RAIN_AO   35
#define RAIN_DO   22
#define LIGHT_AO  33
#define LIGHT_DO  16

const int RPWM = 25;
const int LPWM = 26;
const int R_EN = 27;
const int L_EN = 14;

#define FAN1_PIN  32
#define FAN2_PIN   4

#define FAN_LEDC_FREQ       25
#define FAN_LEDC_RESOLUTION 8

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

float lastTemp     = 0;
float lastHumidity = 0;
int   lastRainVal  = 0;
int   lastLightVal = 0;

enum RainState { RAIN_NONE, RAIN_LIGHT, RAIN_HEAVY };
RainState currentRainState = RAIN_NONE;

String currentFanState = "off";
String currentFanSpeed = "low";

// --- NEW AUTONOMOUS TIMER VARIABLES ---
bool _fanTimerActive = false;
unsigned long _fanTimerStartedAt = 0;
unsigned long _fanTimerDurationMs = 0;

bool _fanStreamBootSkipDone = false;

// ============================================================
// CONCURRENCY — Operation Lock + Last-Write-Wins
// ============================================================
volatile bool _operationLock = false;
unsigned long _operationLockAcquiredAt = 0;

const unsigned long OPERATION_LOCK_TIMEOUT_MS = 1000; 

unsigned long _lastActuatorCmdTs = 0;
unsigned long _lastFanCmdTs      = 0;

bool acquireOperationLock() {
  unsigned long now = millis();
  if (_operationLock && (now - _operationLockAcquiredAt) > OPERATION_LOCK_TIMEOUT_MS) {
    _operationLock = false;
    Serial.println("[LOCK] Stale lock auto-released");
  }
  if (_operationLock) return false;
  _operationLock = true;
  _operationLockAcquiredAt = now;
  return true;
}

void releaseOperationLock() {
  _operationLock = false;
}

// ============================================================
// FORWARD DECLARATIONS
// ============================================================
void extendActuatorInternal(const char* source);
void retractActuatorInternal(const char* source);
void updateCloudState(String state, String source);
void motorStop();
void streamCallback(FirebaseStream data);
void streamTimeoutCallback(bool timeout);
void rejectCommandAndRevert(const char* reason, unsigned long clientTs);
void rejectFanCommandAndRevert(const char* reason, unsigned long clientTs);
void createNotification(String title, String body, String type, String category, String trigger, String action, String priority);
void fanStreamCallback(FirebaseStream data);
void fanStreamTimeoutCallback(bool timeout);
void applyFanSpeed(String speed);
void stopBothFans();
void updateFanCloudState(String state, String speed);
void initFanNodeOnBoot();
void startBLEMode();
void sendBLEStatus();

// ============================================================
// BLE SERVER CALLBACKS
// ============================================================
class SmartRackBLEServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    bleClientConnected = true;
    Serial.println("[BLE] Client connected");
    sendBLEStatus();
  }
  void onDisconnect(BLEServer* pServer) override {
    bleClientConnected = false;
    Serial.println("[BLE] Client disconnected — restarting advertising");
    BLEDevice::startAdvertising();
  }
};

// ============================================================
// BLE COMMAND CALLBACKS — with operation lock
// ============================================================
class SmartRackCommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    String value = pChar->getValue().c_str();
    value.trim();
    Serial.printf("[BLE CMD] Received: %s\n", value.c_str());

    if (!acquireOperationLock()) {
      Serial.println("[BLE LOCK] Command rejected — operation in progress");
      sendBLEStatus(); 
      return;
    }

    if (value == "extend") {
      if (currentRainState == RAIN_HEAVY) {
        Serial.println("[BLE SAFETY] Extend blocked: heavy rain");
        sendBLEStatus();
      } else if (currentFanState == "on") {
        Serial.println("[BLE SAFETY] Extend blocked: fans are on");
        sendBLEStatus();
      } else {
        extendActuatorInternal("ble_command");
        sendBLEStatus();
      }
    }
    else if (value == "retract") {
      retractActuatorInternal("ble_command");
      sendBLEStatus();
    }
    else if (value.startsWith("fan:")) {
      int firstColon  = value.indexOf(':');
      int secondColon = value.indexOf(':', firstColon + 1);
      int thirdColon  = value.indexOf(':', secondColon + 1);

      String fanTarget = "";
      String fanSpeedCmd = currentFanSpeed;
      int durationMins = 0;

      if (secondColon > 0) {
          fanTarget = value.substring(firstColon + 1, secondColon);
          if (thirdColon > 0) {
              fanSpeedCmd = value.substring(secondColon + 1, thirdColon);
              durationMins = value.substring(thirdColon + 1).toInt();
              
              if (durationMins > 120) durationMins = 120; // Max 2 hours
              if (durationMins < 0) durationMins = 0;
          } else {
              fanSpeedCmd = value.substring(secondColon + 1);
          }
      } else {
          fanTarget = value.substring(firstColon + 1);
      }

      if (fanTarget == "on") {
        if (currentPhysicalState == "extended") {
          Serial.println("[BLE SAFETY] Fan blocked: actuator extended");
          sendBLEStatus();
        } else {
          currentFanSpeed = fanSpeedCmd;
          currentFanState = "on";
          applyFanSpeed(currentFanSpeed);

          if (durationMins > 0) {
              _fanTimerActive = true;
              _fanTimerStartedAt = millis();
              _fanTimerDurationMs = durationMins * 60000UL;
              Serial.printf("[BLE FAN] ON speed: %s, timer: %d mins\n", currentFanSpeed.c_str(), durationMins);
          } else {
              _fanTimerActive = false;
              Serial.printf("[BLE FAN] ON speed: %s, continuous\n", currentFanSpeed.c_str());
          }
          sendBLEStatus();
        }
      } else if (fanTarget == "off") {
        currentFanState = "off";
        _fanTimerActive = false; 
        stopBothFans();
        Serial.println("[BLE FAN] OFF");
        sendBLEStatus();
      }
    }
    else if (value == "status") {
      sendBLEStatus();
    }

    releaseOperationLock();
  }
};

// ============================================================
// SEND BLE STATUS
// ============================================================
void sendBLEStatus() {
  if (!bleClientConnected || pStatusChar == nullptr) return;

  float t    = dht.readTemperature();
  float h    = dht.readHumidity();
  int   rain = analogRead(RAIN_AO);

  if (isnan(t)) t = lastTemp;
  if (isnan(h)) h = lastHumidity;

  long remaining = 0;
  if (_fanTimerActive) {
      long elapsed = millis() - _fanTimerStartedAt;
      remaining = (_fanTimerDurationMs - elapsed) / 1000;
      if (remaining < 0) remaining = 0;
  }

  String status = "{";
  status += "\"actuator\":\"" + currentPhysicalState + "\",";
  status += "\"fan\":\"" + currentFanState + "\",";
  status += "\"fanSpeed\":\"" + currentFanSpeed + "\",";
  status += "\"timerRemaining\":" + String(remaining) + ",";
  status += "\"rain\":" + String(rain) + ",";
  status += "\"temp\":" + String(t, 1) + ",";
  status += "\"humidity\":" + String(h, 1) + ",";
  status += "\"rainState\":\"" + String(
    currentRainState == RAIN_HEAVY ? "heavy" :
    currentRainState == RAIN_LIGHT ? "light" : "none"
  ) + "\"";
  status += "}";

  pStatusChar->setValue(status.c_str());
  pStatusChar->notify();
  Serial.printf("[BLE STATUS] Sent: %s\n", status.c_str());
}

// ============================================================
// START BLE MODE
// ============================================================
void startBLEMode() {
  bleMode = true;
  Serial.println("\n[BLE] Starting BLE mode — WiFi unavailable");

  BLEDevice::init(BLE_DEVICE_NAME);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new SmartRackBLEServerCallbacks());

  BLEService* pService = pServer->createService(BLE_SERVICE_UUID);

  pStatusChar = pService->createCharacteristic(
    BLE_STATUS_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pStatusChar->addDescriptor(new BLE2902());
  pStatusChar->setValue("{}");

  pCommandChar = pService->createCharacteristic(
    BLE_COMMAND_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pCommandChar->setCallbacks(new SmartRackCommandCallbacks());

  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(BLE_SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.println("[BLE] Advertising as: " BLE_DEVICE_NAME);
}

// ============================================================
// NOTIFICATION HELPER
// ============================================================
void createNotification(String title, String body, String type, String category, String trigger, String action, String priority) {
  if (!Firebase.ready()) return;

  FirebaseJson notifJson;
  notifJson.set("title",    title);
  notifJson.set("body",     body);
  notifJson.set("time/.sv", "timestamp"); 
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
  notifJson.set("createdAt/.sv", "timestamp"); 

  Firebase.RTDB.pushJSONAsync(&fbdo, NOTIFICATION_PATH.c_str(), &notifJson);
  
  Serial.printf("[NOTIFICATION] %s - %s\n", title.c_str(), body.c_str());
}

// ============================================================
// ACTUATOR STREAM CALLBACK — with lock + last-write-wins
// ============================================================
void streamCallback(FirebaseStream data) {
  Serial.println("[STREAM] Actuator data received");
  String targetState = "";
  unsigned long clientTs = 0;

  if (data.dataType() == "json") {
    FirebaseJson &json = data.jsonObject();
    FirebaseJsonData result;
    if (json.get(result, "target"))          targetState = result.stringValue;
    if (json.get(result, "clientTimestamp")) clientTs = (unsigned long)result.intValue;
  } else if (data.dataType() == "string") {
    targetState = data.stringData();
  }

  targetState.replace("\"", "");
  targetState.trim();
  if (targetState.length() == 0) return;

  if (clientTs > 0 && clientTs <= _lastActuatorCmdTs) {
    Serial.printf("[LWW] Actuator command ts=%lu <= last=%lu — discarded\n", clientTs, _lastActuatorCmdTs);
    return;
  }
  if (clientTs > 0) _lastActuatorCmdTs = clientTs;

  if (!acquireOperationLock()) {
    Serial.println("[LOCK] Actuator command queued — lock held by fan");
    delay(OPERATION_LOCK_TIMEOUT_MS + 10);
    if (!acquireOperationLock()) {
      Serial.println("[LOCK] Actuator command dropped after retry");
      rejectCommandAndRevert("operation_lock_timeout", clientTs);
      return;
    }
  }

  Serial.printf("[STREAM] Actuator target: %s (ts=%lu)\n", targetState.c_str(), clientTs);

  if (targetState == "extended" && currentRainState == RAIN_HEAVY) {
    releaseOperationLock();
    rejectCommandAndRevert("heavy_rain_detected", clientTs);
    createNotification("Command Blocked", "Cannot extend actuator during heavy rain.", "warning", "system", "user_command_rejected", "command_blocked", "high");
    return;
  }

  if (targetState == "extended" && currentFanState == "on") {
    releaseOperationLock();
    rejectCommandAndRevert("fans_are_on", clientTs);
    createNotification("Command Blocked", "Turn off the drying fans before extending the actuator.", "warning", "system", "user_command_rejected", "command_blocked", "high");
    return;
  }

  if      (targetState == "extended"  && currentPhysicalState != "extended")  extendActuatorInternal("app_command");
  else if (targetState == "retracted" && currentPhysicalState != "retracted") retractActuatorInternal("app_command");

  releaseOperationLock();
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("[STREAM] Timeout, resuming...");
}

// ============================================================
// FAN STREAM CALLBACK — with lock + last-write-wins
// ============================================================
void fanStreamCallback(FirebaseStream data) {
  if (!_fanStreamBootSkipDone) {
    _fanStreamBootSkipDone = true;
    Serial.println("[FAN STREAM] Boot echo discarded");
    return;
  }

  Serial.println("[FAN STREAM] Data received");
  String target    = "";
  String speed     = "";
  int durationMins = 0;
  bool hasDuration = false;
  unsigned long clientTs = 0;

  if (data.dataType() == "json") {
    FirebaseJson &json = data.jsonObject();
    FirebaseJsonData result;
    if (json.get(result, "target"))          { target = result.stringValue; target.replace("\"",""); target.trim(); }
    if (json.get(result, "speed"))           { speed  = result.stringValue; speed.replace("\"","");  speed.trim();  }
    if (json.get(result, "duration"))        { durationMins = result.intValue; hasDuration = true; }
    if (json.get(result, "clientTimestamp")) clientTs = (unsigned long)result.intValue;
  } else if (data.dataType() == "string") {
    if      (data.dataPath() == "/target") { target = data.stringData(); target.replace("\"",""); target.trim(); speed = currentFanSpeed; }
    else if (data.dataPath() == "/speed")  { speed  = data.stringData(); speed.replace("\"","");  speed.trim();  target = currentFanState; }
  } else if (data.dataType() == "int" || data.dataType() == "double") {
    if (data.dataPath() == "/duration") { durationMins = data.intData(); hasDuration = true; target = currentFanState; speed = currentFanSpeed; }
  }

  if (speed != "low" && speed != "mid" && speed != "high") speed = currentFanSpeed;

  if (durationMins > 120) durationMins = 120; // Max 2 hours
  if (durationMins < 0) durationMins = 0;

  if (clientTs > 0 && clientTs <= _lastFanCmdTs) {
    Serial.printf("[LWW] Fan command ts=%lu <= last=%lu — discarded duplicate\n", clientTs, _lastFanCmdTs);
    return;
  }
  if (clientTs > 0) _lastFanCmdTs = clientTs;

  if (!acquireOperationLock()) {
    Serial.println("[LOCK] Fan command queued — lock held by actuator");
    delay(OPERATION_LOCK_TIMEOUT_MS + 10);
    if (!acquireOperationLock()) {
      Serial.println("[LOCK] Fan command dropped after retry");
      rejectFanCommandAndRevert("operation_lock_timeout", clientTs);
      return;
    }
  }

  Serial.printf("[FAN STREAM] target=%s speed=%s (ts=%lu)\n", target.c_str(), speed.c_str(), clientTs);

  if (target == "on") {
    if (currentPhysicalState == "extended") {
      releaseOperationLock();
      rejectFanCommandAndRevert("actuator_is_extended", clientTs);
      createNotification("Command Blocked", "Retract the actuator before turning on the drying fans.", "warning", "system", "user_command_rejected", "command_blocked", "high");
      return;
    }
    
    currentFanSpeed = speed;
    currentFanState = "on";
    applyFanSpeed(currentFanSpeed);

    if (hasDuration) {
        if (durationMins > 0) {
            _fanTimerActive = true;
            _fanTimerStartedAt = millis();
            _fanTimerDurationMs = durationMins * 60000UL;
            Serial.printf("[FAN] Timer set for %d mins\n", durationMins);
        } else {
            _fanTimerActive = false;
            Serial.println("[FAN] Continuous mode (timer cleared)");
        }
    }
    
    updateFanCloudState("on", currentFanSpeed);
  }
  else if (target == "off") {
    currentFanState = "off";
    _fanTimerActive = false; 
    stopBothFans();
    updateFanCloudState("off", currentFanSpeed);
  }

  releaseOperationLock();
}

void fanStreamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("[FAN STREAM] Timeout, resuming...");
}

// ============================================================
// FAN HELPERS
// ============================================================
void applyFanSpeed(String speed) {
  uint8_t duty = 100;
  if      (speed == "low")  duty = 100;
  else if (speed == "mid")  duty = 180;
  else if (speed == "high") duty = 255;

  Serial.printf("[FAN PWM] Target speed=%s duty=%d\n", speed.c_str(), duty);

  // --- THE KICKSTART FIX ---
  // 1. Cut power briefly to reset the fan's internal capacitor/chip
  ledcWrite(FAN1_PIN, 0);
  ledcWrite(FAN2_PIN, 0);
  delay(50); 

  // 2. If turning ON (not target 0), give a 100% blast to jumpstart the motor 
  if (duty > 0 && duty < 255) {
    ledcWrite(FAN1_PIN, 255);
    ledcWrite(FAN2_PIN, 255);
    delay(150); // Spin up burst for 150ms
  }

  // 3. Drop down gracefully to the target duty cycle
  ledcWrite(FAN1_PIN, duty);
  ledcWrite(FAN2_PIN, duty);
}

void stopBothFans() {
  ledcWrite(FAN1_PIN, 0);
  ledcWrite(FAN2_PIN, 0);
  Serial.println("[FAN PWM] Both fans stopped");
}

void updateFanCloudState(String state, String speed) {
  if (!Firebase.ready()) return;
  FirebaseJson json;
  json.set("state", state);
  json.set("speed", speed);
  json.set("target", state);
  if (state == "off") json.set("duration", 0); 
  json.set("lastCommandAt/.sv", "timestamp");
  Firebase.RTDB.updateNodeAsync(&fbdo, FAN_PATH.c_str(), &json);
}

void rejectFanCommandAndRevert(const char* reason, unsigned long clientTs) {
  if (!Firebase.ready()) return;
  FirebaseJson json;
  json.set("target",          "off");
  json.set("state",           "off");
  json.set("commandRejected", true);
  json.set("rejectionReason", reason);
  json.set("clientTimestamp", (int)clientTs);
  json.set("lastCommandAt/.sv", "timestamp");
  Firebase.RTDB.updateNodeAsync(&fbdo, FAN_PATH.c_str(), &json);
  Serial.printf("[SAFETY] Fan rejected: %s (ts=%lu)\n", reason, clientTs);
}

void initFanNodeOnBoot() {
  if (!Firebase.ready()) return;
  FirebaseJson json;
  json.set("state", "off");
  json.set("speed", "low");
  json.set("target", "off");
  json.set("duration", 0);
  json.set("lastCommandAt/.sv", "timestamp");
  Firebase.RTDB.updateNodeAsync(&fbdo, FAN_PATH.c_str(), &json);
  Serial.println("[BOOT] fans/ node initialized");
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  delay(1000);

  pinMode(RPWM, OUTPUT); pinMode(LPWM, OUTPUT);
  pinMode(R_EN, OUTPUT); pinMode(L_EN, OUTPUT);
  digitalWrite(R_EN, HIGH); digitalWrite(L_EN, HIGH);
  motorStop();

  ledcAttach(FAN1_PIN, FAN_LEDC_FREQ, FAN_LEDC_RESOLUTION);
  ledcWrite(FAN1_PIN, 0);
  ledcAttach(FAN2_PIN, FAN_LEDC_FREQ, FAN_LEDC_RESOLUTION);
  ledcWrite(FAN2_PIN, 0);

  pinMode(RAIN_DO, INPUT);
  pinMode(LIGHT_DO, INPUT);
  dht.begin();

  // ── WiFi — 15s timeout, then BLE fallback ─────────────────
  Serial.print("[WiFi] Connecting to: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  unsigned long wifiStart = millis();
  const unsigned long WIFI_TIMEOUT_MS = 15000;

  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - wifiStart >= WIFI_TIMEOUT_MS) {
      Serial.println("\n[WiFi] Timeout — switching to BLE mode");
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(100);
      startBLEMode();
      return;
    }
    Serial.print(".");
    delay(500);
  }

  Serial.println("\n[WiFi] Connected! IP: " + WiFi.localIP().toString());

  config.api_key               = API_KEY;
  config.database_url          = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;
  auth.user.email    = "";
  auth.user.password = "";

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("[Firebase] Sign-in OK");
  } else {
    Serial.printf("[Firebase] Sign-in failed: %s\n", config.signer.signupError.message.c_str());
  }

  while (!Firebase.ready()) { Serial.print("."); delay(100); }
  Serial.println("\n[Firebase] Ready!");

  initFanNodeOnBoot();

  if (!Firebase.RTDB.beginStream(&stream, ACTUATOR_PATH.c_str())) {
    Serial.println("Actuator stream error: " + stream.errorReason());
  } else {
    Firebase.RTDB.setStreamCallback(&stream, streamCallback, streamTimeoutCallback);
    Serial.println("[RTDB] Actuator stream started");
  }

  if (!Firebase.RTDB.beginStream(&fanStream, FAN_PATH.c_str())) {
    Serial.println("Fan stream error: " + fanStream.errorReason());
  } else {
    Firebase.RTDB.setStreamCallback(&fanStream, fanStreamCallback, fanStreamTimeoutCallback);
    Serial.println("[RTDB] Fan stream started");
  }
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  unsigned long now = millis();

  // --- AUTONOMOUS FAN TIMER CHECK ---
  if (_fanTimerActive && (now - _fanTimerStartedAt >= _fanTimerDurationMs)) {
      _fanTimerActive = false;
      currentFanState = "off";
      stopBothFans();
      Serial.println("[TIMER] Fan timer expired! Fans OFF.");

      if (!bleMode) {
          updateFanCloudState("off", currentFanSpeed);
      }
      if (bleClientConnected) {
          sendBLEStatus();
      }
  }

  // ── BLE MODE ──────────────────────────────────────────────
  if (bleMode) {
    if (now - lastSensorUpdate >= SENSOR_INTERVAL) {
      lastSensorUpdate = now;
      float t    = dht.readTemperature();
      float h    = dht.readHumidity();
      int   rain = analogRead(RAIN_AO);
      int   light = analogRead(LIGHT_AO);
      if (!isnan(t)) lastTemp     = t;
      if (!isnan(h)) lastHumidity = h;
      lastRainVal = rain; lastLightVal = light;
      Serial.printf("[BLE SENSORS] T:%.1f H:%.1f Rain:%d Light:%d\n", t, h, rain, light);
      if (bleClientConnected) sendBLEStatus();
    }

    int  rainVal = analogRead(RAIN_AO);
    bool rainDig = digitalRead(RAIN_DO) == LOW;

    if (rainVal < RAIN_HEAVY_THRESHOLD) {
      if (currentRainState != RAIN_HEAVY) {
        currentRainState = RAIN_HEAVY;
        if (currentPhysicalState != "retracted") retractActuatorInternal("rain_heavy");
        if (bleClientConnected) sendBLEStatus();
      }
    } else if (rainVal < RAIN_LIGHT_THRESHOLD || rainDig) {
      if (currentRainState != RAIN_LIGHT) {
        currentRainState = RAIN_LIGHT;
        if (currentPhysicalState != "retracted") retractActuatorInternal("rain_light");
        if (bleClientConnected) sendBLEStatus();
      }
    } else {
      currentRainState = RAIN_NONE;
    }

    delay(10);
    return;
  }

  // ── WIFI/FIREBASE MODE ────────────────────────────────────
  if (now - lastSensorUpdate >= SENSOR_INTERVAL) {
    lastSensorUpdate = now;
    float t     = dht.readTemperature();
    float h     = dht.readHumidity();
    int   rain  = analogRead(RAIN_AO);
    int   light = analogRead(LIGHT_AO);
    if (isnan(t)) t = 0;
    if (isnan(h)) h = 0;
    lastTemp = t; lastHumidity = h; lastRainVal = rain; lastLightVal = light;
    Serial.printf("[SENSORS] T:%.1f H:%.1f Rain:%d Light:%d\n", t, h, rain, light);

    FirebaseJson json;
    json.set("temperature", t); json.set("humidity", h);
    json.set("rainAO", rain);   json.set("light", light);
    json.set("updatedAt/.sv", "timestamp");
    if (Firebase.ready()) Firebase.RTDB.updateNodeAsync(&fbdo, SENSOR_PATH.c_str(), &json);
  }

  int  rainVal = analogRead(RAIN_AO);
  bool rainDig = digitalRead(RAIN_DO) == LOW;

  if (rainVal < RAIN_HEAVY_THRESHOLD) {
    if (currentRainState != RAIN_HEAVY) {
      currentRainState = RAIN_HEAVY;
      if (currentPhysicalState != "retracted") {
        retractActuatorInternal("rain_heavy");
        createNotification("Heavy Rain Detected!", "Actuator retracted due to heavy rainfall.", "alert", "weather", "rain_sensor", "actuator_retracted", "high");
      }
    }
  } else if (rainVal < RAIN_LIGHT_THRESHOLD || rainDig) {
    if (currentRainState != RAIN_LIGHT) {
      currentRainState = RAIN_LIGHT;
      if (currentPhysicalState != "retracted") {
        retractActuatorInternal("rain_light");
        createNotification("Rain Detected", "Actuator retracted due to light rain.", "info", "weather", "rain_sensor", "actuator_retracted", "medium");
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
  json.set("state", state); json.set("source", source);
  json.set("target", state); 
  json.set("lastCommandAt/.sv", "timestamp");
  Firebase.RTDB.updateNodeAsync(&fbdo, ACTUATOR_PATH.c_str(), &json);
}

void rejectCommandAndRevert(const char* reason, unsigned long clientTs) {
  if (!Firebase.ready()) return;
  FirebaseJson json;
  json.set("target",          "retracted");
  json.set("state",           currentPhysicalState);
  json.set("commandRejected", true);
  json.set("rejectionReason", reason);
  json.set("clientTimestamp", (int)clientTs);  
  json.set("lastCommandAt/.sv", "timestamp");
  Firebase.RTDB.updateNodeAsync(&fbdo, ACTUATOR_PATH.c_str(), &json);
  Serial.printf("[SAFETY] Actuator rejected: %s (ts=%lu)\n", reason, clientTs);
}

void extendActuatorInternal(const char* source) {
  if (currentPhysicalState == "extended") return;
  motorStop(); delay(100);
  analogWrite(RPWM, 255); analogWrite(LPWM, 0);
  currentPhysicalState = "extended";
  if (!bleMode) updateCloudState("extended", source);
  Serial.println("[ACTUATOR] Extended");
}

void retractActuatorInternal(const char* source) {
  if (currentPhysicalState == "retracted") return;
  motorStop(); delay(100);
  analogWrite(RPWM, 0); analogWrite(LPWM, 255);
  currentPhysicalState = "retracted";
  if (!bleMode) updateCloudState("retracted", source);
  Serial.println("[ACTUATOR] Retracted");
}