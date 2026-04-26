#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include "DHT.h"
#include <Wire.h>
#include "RTClib.h"
#include "time.h"

// ===== STORAGE ===
Preferences prefs;

// ===== FIREBASE =====
char apiKey[100] = "";
char databaseUrl[100] = "";

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ===== RTC =====
RTC_DS3231 rtc;

// ===== NTP =====
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600;
const int daylightOffset_sec = 0;

unsigned long lastSync = 0;
const unsigned long syncInterval = 3600000; // 1 jam

// ===== DHT =====
#define DHTPIN 33
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// ===== RELAY =====
const int relayHeater = 26;
const int relayFan = 27;

// ===== LED =====
const int ledHeater = 25;

// 🔥 RELAY LOGIC
#define RELAY_ON LOW
#define RELAY_OFF HIGH

// ===== VARIABLE =====
int startHour, startMinute;
int endHour, endMinute;
float targetTemp;

int mode = 0;
int manualHeater = 0;
int manualFan = 0;

float tempOffset = 0.0;
float humOffset  = 0.0;

// ===== WIFI MANAGER =====
void setup_wifi() {
  WiFiManager wm;

  WiFiManagerParameter custom_api("api", "Firebase API Key", apiKey, 100);
  WiFiManagerParameter custom_db("db", "Database URL", databaseUrl, 100);

  wm.addParameter(&custom_api);
  wm.addParameter(&custom_db);

  if (!wm.autoConnect("ESP32-SETUP", "12345678")) {
    ESP.restart();
  }

  prefs.begin("config", false);
  prefs.putString("api", custom_api.getValue());
  prefs.putString("db", custom_db.getValue());
  prefs.end();

  strcpy(apiKey, custom_api.getValue());
  strcpy(databaseUrl, custom_db.getValue());
}

// ===== LOAD CONFIG =====
void loadConfig() {
  prefs.begin("config", true);
  String api = prefs.getString("api", "");
  String db = prefs.getString("db", "");
  prefs.end();

  api.toCharArray(apiKey, 100);
  db.toCharArray(databaseUrl, 100);
}

// ===== SYNC RTC =====
void syncRTC() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    rtc.adjust(DateTime(
      timeinfo.tm_year + 1900,
      timeinfo.tm_mon + 1,
      timeinfo.tm_mday,
      timeinfo.tm_hour,
      timeinfo.tm_min,
      timeinfo.tm_sec
    ));
    Serial.println("✅ RTC Sync NTP");
  } else {
    Serial.println("❌ NTP gagal");
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(2000);

  pinMode(relayHeater, OUTPUT);
  pinMode(relayFan, OUTPUT);
  pinMode(ledHeater, OUTPUT);

  digitalWrite(relayHeater, RELAY_OFF);
  digitalWrite(relayFan, RELAY_OFF);
  digitalWrite(ledHeater, LOW);

  dht.begin();
  Wire.begin(13, 14);

  if (!rtc.begin()) {
    Serial.println("❌ RTC error");
    while (1);
  }

  loadConfig();

  if (strlen(apiKey) == 0) {
    setup_wifi();
  } else {
    WiFi.begin();
    if (WiFi.waitForConnectResult() != WL_CONNECTED) {
      setup_wifi();
    }
  }

  // ===== FIREBASE =====
  config.api_key = apiKey;
  config.database_url = databaseUrl;
  config.signer.test_mode = true;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // 🔥 SYNC RTC AWAL
  syncRTC();
  lastSync = millis();

  Serial.println("🔥 SYSTEM READY");
}

// ===== LOOP =====
void loop() {

  // ===== WIFI CHECK =====
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
  }

  // ===== SYNC RTC PERIODIC =====
  if (WiFi.status() == WL_CONNECTED && millis() - lastSync > syncInterval) {
    syncRTC();
    lastSync = millis();
  }

  // ===== GET FIREBASE =====
  Firebase.RTDB.getInt(&fbdo, "/control/start_hour"); startHour = fbdo.intData();
  Firebase.RTDB.getInt(&fbdo, "/control/start_minute"); startMinute = fbdo.intData();
  Firebase.RTDB.getInt(&fbdo, "/control/end_hour"); endHour = fbdo.intData();
  Firebase.RTDB.getInt(&fbdo, "/control/end_minute"); endMinute = fbdo.intData();
  Firebase.RTDB.getFloat(&fbdo, "/control/target_temp"); targetTemp = fbdo.floatData();
  Firebase.RTDB.getInt(&fbdo, "/control/mode"); mode = fbdo.intData();

  Firebase.RTDB.getInt(&fbdo, "/manual/heater"); manualHeater = fbdo.intData();
  Firebase.RTDB.getInt(&fbdo, "/manual/fan"); manualFan = fbdo.intData();

  // ===== TIME =====
  DateTime now = rtc.now();
  int currentTime = now.hour() * 60 + now.minute();
  int startTime = startHour * 60 + startMinute;
  int endTime = endHour * 60 + endMinute;

  bool active = (startTime <= endTime)
    ? (currentTime >= startTime && currentTime <= endTime)
    : (currentTime >= startTime || currentTime <= endTime);

  // ===== SENSOR =====
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();

  if (isnan(temp) || isnan(hum)) {
    Serial.println("❌ DHT error");
    delay(2000);
    return;
  }

  // ===== CONTROL =====
  bool heaterState = false;

  if (mode == 1) {
    // MANUAL
    heaterState = manualHeater;
    digitalWrite(relayFan, manualFan ? RELAY_ON : RELAY_OFF);

  } else {
    // AUTO
    if (active) {
      heaterState = (temp < targetTemp);
      digitalWrite(relayFan, temp > targetTemp + 2 ? RELAY_ON : RELAY_OFF);
    } else {
      heaterState = false;
      digitalWrite(relayFan, RELAY_OFF);
    }
  }

  // ===== APPLY HEATER =====
  digitalWrite(relayHeater, heaterState ? RELAY_ON : RELAY_OFF);
  digitalWrite(ledHeater, heaterState ? HIGH : LOW);

  // ===== SEND DATA =====
  Firebase.RTDB.setFloat(&fbdo, "/sensor/temperature", temp);
  Firebase.RTDB.setFloat(&fbdo, "/sensor/humidity", hum);

  Firebase.RTDB.setInt(&fbdo, "/relay/heater", heaterState);
  Firebase.RTDB.setInt(&fbdo, "/relay/fan", digitalRead(relayFan) == RELAY_ON);

  // ===== DEBUG =====
  Serial.println("===== SYSTEM =====");
  Serial.print("Time: "); Serial.print(now.hour()); Serial.print(":"); Serial.println(now.minute());
  Serial.print("Temp: "); Serial.println(temp);
  Serial.print("Mode: "); Serial.println(mode == 1 ? "MANUAL" : "AUTO");
  Serial.print("Active: "); Serial.println(active);
  Serial.println("==================");

  delay(3000);
}