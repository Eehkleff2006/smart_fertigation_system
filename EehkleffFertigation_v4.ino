/*
 * ============================================================
 *   EEHKLEFF SMART FERTIGATION SYSTEM — v4
 *   Eehkleff Technologies
 * ============================================================
 *
 *  HARDWARE:
 *   - ESP32 Dev Board
 *   - Capacitive Soil Moisture Sensor  → GPIO34
 *   - DHT11 Temp/Humidity              → GPIO4
 *   - SSD1306 OLED 128x64 (I2C)       → GPIO21(SDA), GPIO22(SCL)
 *   - DS1302 RTC                       → CLK=GPIO18, DAT=GPIO19, RST=GPIO5
 *   - 2-Channel Relay Module
 *       Relay 1 → Pump 1 (Water Tank Outlet)        → GPIO25
 *       Relay 2 → Pump 2 (Fertilizer Tank Outlet)   → GPIO26
 *
 *  POWER:
 *   - 2x 18650 batteries in series → Buck converter → 5V
 *   - Buck converter must be rated minimum 2A
 *
 *  LIBRARIES (install via Arduino Library Manager):
 *   - Firebase ESP32 Client (by Mobizt) v4.4.17
 *   - DHT sensor library (by Adafruit)
 *   - Adafruit GFX Library
 *   - Adafruit SSD1306
 *   - Rtc by Makuna (for DS1302)
 *   - NTPClient (by Fabrice Weinberg)
 * ============================================================
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <FirebaseESP32.h>
#include <NTPClient.h>
#include <Wire.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>

// ── WiFi Credentials ───────────────────────────────────────
#define WIFI_SSID       "LET NETWORK"
#define WIFI_PASSWORD   "technology123455"

// ── Firebase Credentials ───────────────────────────────────
#define FIREBASE_HOST   "https://eehkleff-smart-fertigation-default-rtdb.firebaseio.com"
#define FIREBASE_AUTH   "AIzaSyDxB30_SxNG_wBKspaxf6oUyU-X4Ixs2Hs"

// ── Firebase Paths ─────────────────────────────────────────
#define PATH_SOIL         "/sensorData/soilMoisture"
#define PATH_SOIL_STATUS  "/sensorData/soilStatus"
#define PATH_TEMP         "/sensorData/temperature"
#define PATH_HUMIDITY     "/sensorData/humidity"
#define PATH_PUMP1        "/sensorData/pump1"
#define PATH_PUMP2        "/sensorData/pump2"
#define PATH_SYS_STATUS   "/sensorData/systemStatus"
#define PATH_RTC_TIME     "/sensorData/rtcTime"
#define PATH_NTP_TIME     "/sensorData/ntpTime"
#define PATH_HEARTBEAT    "/sensorData/heartbeat"
#define PATH_CMD_ACTION   "/commands/action"
#define PATH_CMD_PUMP1    "/commands/pump1"
#define PATH_CMD_PUMP2    "/commands/pump2"
#define PATH_SCHED_HOUR   "/schedule/hour"
#define PATH_SCHED_MIN    "/schedule/minute"
#define PATH_WATER_DUR    "/schedule/waterDuration"
#define PATH_FERT_DUR     "/schedule/fertDuration"
#define PATH_MODE         "/schedule/mode"

// ── Pin Definitions ────────────────────────────────────────
#define SOIL_PIN      34
#define DHT_PIN       4
#define RELAY_PUMP1   25
#define RELAY_PUMP2   26
#define DS1302_CLK    18
#define DS1302_DAT    19
#define DS1302_RST    5

// ── Relay Logic (Active LOW) ───────────────────────────────
#define RELAY_ON    LOW
#define RELAY_OFF   HIGH

// ── OLED ───────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ── DHT11 ──────────────────────────────────────────────────
#define DHT_TYPE DHT11
DHT dht(DHT_PIN, DHT_TYPE);

// ── DS1302 RTC ─────────────────────────────────────────────
ThreeWire myWire(DS1302_DAT, DS1302_CLK, DS1302_RST);
RtcDS1302<ThreeWire> rtc(myWire);

// ── NTP (Nigeria UTC+1 = 3600s offset) ────────────────────
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600, 60000);

// ── Firebase Objects ───────────────────────────────────────
FirebaseData fbData;
FirebaseData fbCmd;
FirebaseAuth fbAuth;
FirebaseConfig fbConfig;

// ── Soil Moisture Thresholds ───────────────────────────────
#define SOIL_DRY    2800
#define SOIL_WET    1500

// ── Timing ─────────────────────────────────────────────────
#define PUMP2_START_DELAY   0   // Pump 2 starts 10s after Pump 1
#define COOLDOWN_MS         300000  // 5 minutes cooldown

// ── System States ──────────────────────────────────────────
enum SystemState {
  STATE_IDLE,
  STATE_WATERING,
  STATE_COOLDOWN
};
SystemState currentState = STATE_IDLE;

// ── Control Modes ──────────────────────────────────────────
int controlMode = 1;

// ── Schedule ───────────────────────────────────────────────
int  schedHour      = 6;
int  schedMinute    = 0;
int  waterDuration  = 30000;
int  fertDuration   = 15000;
bool schedTriggered = false;

// ── Sensor Values ──────────────────────────────────────────
int   soilRaw     = 0;
int   soilPct     = 0;
float temperature = 0.0;
float humidity    = 0.0;

// ── Pump States ────────────────────────────────────────────
bool pump1State   = false;
bool pump2State   = false;
bool pump2Pending = false;
bool pump2Running = false;

// ── Heartbeat counter ──────────────────────────────────────
int heartbeatCount = 1000;

// ── Timing Variables ───────────────────────────────────────
unsigned long stateStart       = 0;
unsigned long pump2Start       = 0;
unsigned long lastSensor       = 0;
unsigned long lastFirebasePush = 0;
unsigned long lastFirebaseRead = 0;
unsigned long lastOLED         = 0;
unsigned long lastNTPSync      = 0;

#define SENSOR_INTERVAL     5000
#define FIREBASE_PUSH_INT   2000
#define FIREBASE_READ_INT   2000
#define OLED_INTERVAL       2000
#define NTP_SYNC_INTERVAL   3600000

String systemStatusMsg = "IDLE";

// ══════════════════════════════════════════════════════════
//  FORWARD DECLARATIONS
//  Needed because setPump1/setPump2 call updateOLED
//  which is defined later in the file
// ══════════════════════════════════════════════════════════
void updateOLED();
void startWatering();

// ══════════════════════════════════════════════════════════
//  HELPERS
// ══════════════════════════════════════════════════════════
String getSoilStatus(int pct) {
  if (pct <= 20)  return "Dry";
  if (pct <= 45)  return "Low";
  if (pct <= 75)  return "Optimal";
  return "Saturated";
}

// Strips all non-letter, non-digit characters from string
// Fixes Firebase double-quote wrapping issue e.g. ""start"" → start
String cleanString(String s) {
  s.trim();
  String result = "";
  for (int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (isAlphaNumeric(c)) result += c;
  }
  return result;
}

String getRTCTimeString() {
  RtcDateTime now = rtc.GetDateTime();
  char buf[12];
  sprintf(buf, "%02d.%02d.%02d",
    now.Hour(), now.Minute(), now.Second());
  return String(buf);
}

// ── Set Pump 1 state ───────────────────────────────────────
void setPump1(bool on) {
  pump1State = on;
  digitalWrite(RELAY_PUMP1, on ? RELAY_ON : RELAY_OFF);
  Firebase.setString(fbData, PATH_PUMP1, on ? "ON" : "OFF");
  if (on) {
    systemStatusMsg = "WATERING";
    Firebase.setString(fbData, PATH_SYS_STATUS, "WATERING");
  } else if (!pump2State) {
    systemStatusMsg = "IDLE";
    Firebase.setString(fbData, PATH_SYS_STATUS, "IDLE");
  }
  Serial.print("Pump 1: "); Serial.println(on ? "ON" : "OFF");
  updateOLED();
}

// ── Set Pump 2 state ───────────────────────────────────────
void setPump2(bool on) {
  pump2State = on;
  digitalWrite(RELAY_PUMP2, on ? RELAY_ON : RELAY_OFF);
  Firebase.setString(fbData, PATH_PUMP2, on ? "ON" : "OFF");
  if (on) {
    systemStatusMsg = "FERTIGATION";
    Firebase.setString(fbData, PATH_SYS_STATUS, "FERTIGATION");
  } else if (!pump1State) {
    systemStatusMsg = "IDLE";
    Firebase.setString(fbData, PATH_SYS_STATUS, "IDLE");
  }
  Serial.print("Pump 2: "); Serial.println(on ? "ON" : "OFF");
  updateOLED();
}

void allPumpsOff() {
  pump1State   = false;
  pump2State   = false;
  pump2Pending = false;
  pump2Running = false;
  digitalWrite(RELAY_PUMP1, RELAY_OFF);
  digitalWrite(RELAY_PUMP2, RELAY_OFF);
  Firebase.setString(fbData, PATH_PUMP1, "OFF");
  Firebase.setString(fbData, PATH_PUMP2, "OFF");
}

void startWatering() { 
  currentState    = STATE_WATERING;
  stateStart      = millis();
  pump2Pending    = false;
  pump2Running    = true;
  pump2Start      = millis();
  pump2State      = true;
  digitalWrite(RELAY_PUMP2, RELAY_ON);
  Firebase.setString(fbData, PATH_PUMP2, "ON");
  pump1State      = true;
  digitalWrite(RELAY_PUMP1, RELAY_ON);
  Firebase.setString(fbData, PATH_PUMP1, "ON");
  systemStatusMsg = "FERTIGATION";
  Firebase.setString(fbData, PATH_SYS_STATUS, "FERTIGATION");
  Firebase.setString(fbCmd,  PATH_CMD_ACTION,  "none");
  Serial.println("Watering cycle started");
  updateOLED();
}

// ══════════════════════════════════════════════════════════
//  READ SENSORS
// ══════════════════════════════════════════════════════════
void readSensors() {
  soilRaw = analogRead(SOIL_PIN);
  soilPct = map(soilRaw, SOIL_WET, SOIL_DRY, 100, 0);
  soilPct = constrain(soilPct, 0, 100);

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) temperature = t;
  if (!isnan(h)) humidity    = h;
}

// ══════════════════════════════════════════════════════════
//  PUSH TO FIREBASE
// ══════════════════════════════════════════════════════════
void pushToFirebase() {
  if (Firebase.setInt(fbData, PATH_SOIL, soilPct)) {
    Serial.println("Soil pushed OK");
  } else {
    Serial.println("Soil push FAILED: " + fbData.errorReason());
  }

  Firebase.setString(fbData, PATH_SOIL_STATUS, getSoilStatus(soilPct));
  Firebase.setFloat (fbData, PATH_TEMP,        temperature);
  Firebase.setFloat (fbData, PATH_HUMIDITY,    humidity);
  Firebase.setString(fbData, PATH_PUMP1,       pump1State ? "ON" : "OFF");
  Firebase.setString(fbData, PATH_PUMP2,       pump2State ? "ON" : "OFF");
  Firebase.setString(fbData, PATH_SYS_STATUS,  systemStatusMsg);

  // Time with dots — avoids Firebase path/number parsing issues
  String rtcStr = getRTCTimeString();
  Firebase.setString(fbData, PATH_RTC_TIME, rtcStr);

  String ntpStr = timeClient.getFormattedTime();
  ntpStr.replace(":", ".");
  Firebase.setString(fbData, PATH_NTP_TIME, ntpStr);

  // Heartbeat — increments every push so app can detect offline state
  heartbeatCount++;
  Firebase.setInt(fbData, PATH_HEARTBEAT, heartbeatCount);
}

// ══════════════════════════════════════════════════════════
//  CLEAN STRING HELPER — strips Firebase quote wrapping
// ══════════════════════════════════════════════════════════
void readCommands() {
  Serial.println("Reading commands...");

  // ── Action command ─────────────────────────────────────
  if (Firebase.getString(fbCmd, PATH_CMD_ACTION)) {
    String action = cleanString(fbCmd.stringData());
    Serial.print("Action read: [");
    Serial.print(action);
    Serial.print("] State: ");
    Serial.println(currentState);

    if (action == "start" && currentState == STATE_IDLE) {
      startWatering();
    }
    else if (action == "stop") {
      allPumpsOff();
      currentState    = STATE_IDLE;
      stateStart      = millis();
      systemStatusMsg = "IDLE";
      Firebase.setString(fbData, PATH_SYS_STATUS, "IDLE");
      Firebase.setString(fbCmd,  PATH_CMD_ACTION,  "none");
      Serial.println("Stopped by app");
      updateOLED();
    }
  }

  // ── Pump 1 individual command ──────────────────────────
  if (Firebase.getString(fbCmd, PATH_CMD_PUMP1)) {
    String cmd = cleanString(fbCmd.stringData());
    if (cmd == "on")  { setPump1(true);  Firebase.setString(fbCmd, PATH_CMD_PUMP1, "none"); }
    if (cmd == "off") { setPump1(false); Firebase.setString(fbCmd, PATH_CMD_PUMP1, "none"); }
  }

  // ── Pump 2 individual command ──────────────────────────
  if (Firebase.getString(fbCmd, PATH_CMD_PUMP2)) {
    String cmd = cleanString(fbCmd.stringData());
    if (cmd == "on")  { setPump2(true);  Firebase.setString(fbCmd, PATH_CMD_PUMP2, "none"); }
    if (cmd == "off") { setPump2(false); Firebase.setString(fbCmd, PATH_CMD_PUMP2, "none"); }
  }

  // ── Schedule settings — use getString + cleanString + toInt ──
  if (Firebase.getString(fbCmd, PATH_SCHED_HOUR))
    schedHour = cleanString(fbCmd.stringData()).toInt();

  if (Firebase.getString(fbCmd, PATH_SCHED_MIN))
    schedMinute = cleanString(fbCmd.stringData()).toInt();

  if (Firebase.getString(fbCmd, PATH_WATER_DUR))
    waterDuration = cleanString(fbCmd.stringData()).toInt() * 1000;

  if (Firebase.getString(fbCmd, PATH_FERT_DUR)) {
    fertDuration = cleanString(fbCmd.stringData()).toInt() * 1000;
    Serial.print("Fert duration set to: ");
    Serial.println(fertDuration);
  }

  if (Firebase.getString(fbCmd, PATH_MODE))
    controlMode = cleanString(fbCmd.stringData()).toInt();
}

// ══════════════════════════════════════════════════════════
//  CHECK DAILY SCHEDULE
// ══════════════════════════════════════════════════════════
void checkSchedule() {
  RtcDateTime now = rtc.GetDateTime();
  if (now.Hour()   == schedHour   &&
      now.Minute() == schedMinute &&
      now.Second()  < 10          &&
      !schedTriggered             &&
      currentState == STATE_IDLE) {
    schedTriggered = true;
    startWatering();
    Serial.println("Daily schedule triggered");
  }
  if (now.Minute() != schedMinute) {
    schedTriggered = false;
  }
}

// ══════════════════════════════════════════════════════════
//  UPDATE OLED
// ══════════════════════════════════════════════════════════
void updateOLED() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("RTC: ");
  display.println(getRTCTimeString());

  display.print("Soil: ");
  display.print(soilPct);
  display.print("% ");
  display.println(getSoilStatus(soilPct));

  display.print("T:");
  display.print(temperature, 1);
  display.print("C  H:");
  display.print(humidity, 0);
  display.println("%");

  display.print("P1:");
  display.print(pump1State ? "ON " : "OFF");
  display.print("  P2:");
  display.println(pump2State ? "ON" : "OFF");

  display.print("Mode: ");
  if      (controlMode == 1) display.println("SENSOR");
  else if (controlMode == 2) display.println("TIME");
  else                       display.println("COMBINED");

  display.println(systemStatusMsg);
  display.display();
}

// ══════════════════════════════════════════════════════════
//  STATE MACHINE
// ══════════════════════════════════════════════════════════
void runStateMachine() {
  unsigned long now     = millis();
  unsigned long elapsed = now - stateStart;

  switch (currentState) {


    
    case STATE_IDLE:
      if (!pump1State && !pump2State) {
        systemStatusMsg = "IDLE";
      }
      if (controlMode == 1 && soilRaw > SOIL_DRY) {
        startWatering();
      }
      break;

    case STATE_WATERING:
      

      if (pump2Running && (now - pump2Start) >= (unsigned long)fertDuration) {
          pump2Running    = false;
          pump2State      = false;
          digitalWrite(RELAY_PUMP2, RELAY_OFF);
          Firebase.setString(fbData, PATH_PUMP2, "OFF");
          systemStatusMsg = "WATERING";
          Firebase.setString(fbData, PATH_SYS_STATUS, "WATERING");
          Serial.println("Pump 2 OFF — continuing water only");
          updateOLED();
}

      // Mode 1 or 3 — stop Pump 1 when soil saturated
      if (controlMode == 1 || controlMode == 3) {
        if (soilRaw <= SOIL_WET) {
          allPumpsOff();
          currentState    = STATE_COOLDOWN;
          stateStart      = now;
          systemStatusMsg = "COOLDOWN";
          Firebase.setString(fbData, PATH_SYS_STATUS, "COOLDOWN");
          Serial.println("Saturated — cooldown");
          updateOLED();
        }
      }

      // Mode 2 — stop Pump 1 after waterDuration
      if (controlMode == 2) {
        if (elapsed >= (unsigned long)waterDuration) {
          allPumpsOff();
          currentState    = STATE_COOLDOWN;
          stateStart      = now;
          systemStatusMsg = "COOLDOWN";
          Firebase.setString(fbData, PATH_SYS_STATUS, "COOLDOWN");
          Serial.println("Duration done — cooldown");
          updateOLED();
        }
      }
      break;

    case STATE_COOLDOWN:
      allPumpsOff();
      if (elapsed >= COOLDOWN_MS) {
        currentState    = STATE_IDLE;
        stateStart      = now;
        systemStatusMsg = "IDLE";
        Firebase.setString(fbData, PATH_SYS_STATUS, "IDLE");
        Serial.println("Cooldown done — IDLE");
        updateOLED();
      }
      break;
  }
}

// ══════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  // Relay pins OFF immediately — prevents pump firing on boot
  pinMode(RELAY_PUMP1, OUTPUT); digitalWrite(RELAY_PUMP1, RELAY_OFF);
  pinMode(RELAY_PUMP2, OUTPUT); digitalWrite(RELAY_PUMP2, RELAY_OFF);

  // DHT11
  dht.begin();

  // OLED
  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed");
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Eehkleff Fertigation");
  display.println("Initialising...");
  display.display();

  // DS1302 RTC
  rtc.Begin();
  RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);
  if (!rtc.IsDateTimeValid() || !rtc.GetIsRunning()) {
    Serial.println("RTC not running — setting compile time");
    rtc.SetIsRunning(true);
    rtc.SetDateTime(compiled);
  }

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  display.println("Connecting WiFi...");
  display.display();

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());

    // NTP
    timeClient.begin();
    timeClient.update();

    // Sync RTC with NTP
    rtc.SetDateTime(RtcDateTime(timeClient.getEpochTime()));
    Serial.println("RTC synced with NTP");

    // Firebase
    fbConfig.database_url = FIREBASE_HOST;
    fbConfig.signer.tokens.legacy_token = FIREBASE_AUTH;
    Firebase.begin(&fbConfig, &fbAuth);
    Firebase.reconnectWiFi(true);

    if (Firebase.ready()) {
      Serial.println("Firebase connected successfully");
    } else {
      Serial.println("Firebase connection FAILED");
      Serial.println(fbData.errorReason());
    }

    // Initialise command nodes
    Firebase.setString(fbCmd, PATH_CMD_ACTION, "none");
    Firebase.setString(fbCmd, PATH_CMD_PUMP1,  "none");
    Firebase.setString(fbCmd, PATH_CMD_PUMP2,  "none");

    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Eehkleff Fertigation");
    display.println("WiFi Connected");
    display.println("Firebase Ready");
    display.print("IP: ");
    display.println(WiFi.localIP());
    display.display();

  } else {
    Serial.println();
    Serial.println("WiFi failed — running offline");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Eehkleff Fertigation");
    display.println("WiFi FAILED");
    display.println("Running offline");
    display.println("Sensor mode only");
    display.display();
  }

  delay(2000);
  Serial.println("System Ready");
}

// ══════════════════════════════════════════════════════════
//  MAIN LOOP
// ══════════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  // Read sensors every 5 seconds
  if (now - lastSensor >= SENSOR_INTERVAL) {
    readSensors();
    lastSensor = now;
  }

  // NTP sync every hour
  if (WiFi.status() == WL_CONNECTED &&
      now - lastNTPSync >= NTP_SYNC_INTERVAL) {
    timeClient.update();
    rtc.SetDateTime(RtcDateTime(timeClient.getEpochTime()));
    lastNTPSync = now;
    Serial.println("NTP synced to RTC");
  }

  // Push to Firebase every 2 seconds
  if (WiFi.status() == WL_CONNECTED &&
      now - lastFirebasePush >= FIREBASE_PUSH_INT) {
    pushToFirebase();
    lastFirebasePush = now;
  }

  // Read commands every 2 seconds
  if (WiFi.status() == WL_CONNECTED &&
      now - lastFirebaseRead >= FIREBASE_READ_INT) {
    readCommands();
    lastFirebaseRead = now;
  }

  // Check daily schedule (Modes 2 and 3)
  if (controlMode == 2 || controlMode == 3) {
    checkSchedule();
  }

  // Run state machine
  runStateMachine();

  // Update OLED every 2 seconds
  if (now - lastOLED >= OLED_INTERVAL) {
    updateOLED();
    lastOLED = now;
  }
}
