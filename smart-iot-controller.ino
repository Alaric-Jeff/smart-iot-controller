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
const String ACTUATOR_PATH = "/devices/" + DEVICE_ID + "/actuator";
const String SENSOR_PATH = "/devices/" + DEVICE_ID + "/sensors";

// ============================================================
// PIN DEFINITIONS
// ============================================================
#define DHTPIN 23
#define DHTTYPE DHT11
#define RAIN_AO 35
#define RAIN_DO 22
#define LIGHT_AO 33
#define LIGHT_DO 16

const int RPWM = 25;
const int LPWM = 26;
const int R_EN = 27;
const int L_EN = 14;

#define RAIN_LIGHT_THRESHOLD  2000
#define RAIN_HEAVY_THRESHOLD  800

// ============================================================
// GLOBAL VARIABLES
// ============================================================
DHT dht(DHTPIN, DHTTYPE);

FirebaseData fbdo;
FirebaseData stream;
FirebaseAuth auth;
FirebaseConfig config;

String currentPhysicalState = "retracted";
unsigned long lastSensorUpdate = 0;
const unsigned long SENSOR_INTERVAL = 10000;

enum RainState { RAIN_NONE, RAIN_LIGHT, RAIN_HEAVY };
RainState currentRainState = RAIN_NONE;

// ============================================================
// FORWARD DECLARATIONS
// ============================================================
void extendActuatorInternal(const char* source);
void retractActuatorInternal(const char* source);
void updateCloudState(String state, String source);
void motorStop();
void streamCallback(FirebaseStream data);
void streamTimeoutCallback(bool timeout);

// ============================================================
// FIREBASE STREAM CALLBACK
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

    if (targetState == "extended" && currentPhysicalState != "extended") {
      extendActuatorInternal("app_command");
    } 
    else if (targetState == "retracted" && currentPhysicalState != "retracted") {
      retractActuatorInternal("app_command");
    }
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) {
    Serial.println("[STREAM] Timeout, resuming...");
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

  // ANONYMOUS AUTHENTICATION
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;
  
  // Sign in anonymously
  auth.user.email = "";
  auth.user.password = "";

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Sign in anonymously
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

  if (!Firebase.RTDB.beginStream(&stream, ACTUATOR_PATH.c_str())) {
    Serial.println("Stream error: " + stream.errorReason());
  } else {
    Firebase.RTDB.setStreamCallback(&stream, streamCallback, streamTimeoutCallback);
    Serial.println("[RTDB] Stream started on: " + ACTUATOR_PATH);
  }
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  unsigned long now = millis();

  if (now - lastSensorUpdate >= SENSOR_INTERVAL) {
    lastSensorUpdate = now;
    
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    int rain = analogRead(RAIN_AO);
    int light = analogRead(LIGHT_AO);
    
    if (isnan(t)) t = 0;
    if (isnan(h)) h = 0;

    Serial.printf("[SENSORS] T:%.1f H:%.1f Rain:%d\n", t, h, rain);

    FirebaseJson json;
    json.set("temperature", t);
    json.set("humidity", h);
    json.set("rainAO", rain);
    json.set("light", light);
    json.set("updatedAt", (int)now);

    if (Firebase.ready()) {
      Firebase.RTDB.updateNodeAsync(&fbdo, SENSOR_PATH.c_str(), &json);
    }
  }

  int rainVal = analogRead(RAIN_AO);
  bool rainDig = digitalRead(RAIN_DO) == LOW;
  
  if (rainVal < RAIN_HEAVY_THRESHOLD) {
    if (currentRainState != RAIN_HEAVY) {
      currentRainState = RAIN_HEAVY;
      if (currentPhysicalState != "extended") {
        Serial.println("[RAIN] HEAVY! Extending...");
        extendActuatorInternal("rain_heavy");
      }
    }
  } else if (rainVal < RAIN_LIGHT_THRESHOLD || rainDig) {
    if (currentRainState != RAIN_LIGHT) {
      currentRainState = RAIN_LIGHT;
      if (currentPhysicalState != "retracted") {
        Serial.println("[RAIN] LIGHT! Retracting...");
        retractActuatorInternal("rain_light");
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
  json.set("state", state);
  json.set("source", source);
  json.set("lastCommandAt", (int)millis());
  
  Firebase.RTDB.updateNodeAsync(&fbdo, ACTUATOR_PATH.c_str(), &json);
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