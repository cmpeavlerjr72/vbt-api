/*
  VBT_RFID_IMU_v6.ino
  ESP32-S3 + QMI8658 IMU + TFT_eSPI Display + 3 Buttons
  + WiFi + Button-based Player/Exercise Selection + HTTP Upload

  Based on v5 — adds:
    - WiFi connection and HTTP communication with backend
    - Player selection via buttons (scroll through roster fetched from backend)
    - Exercise selection via buttons (scroll through hardcoded list)
    - POST set/rep data to backend after each set completes
    - RFID removed for v6 (button selection replaces RFID gate)

  Hardware:
    IMU:     I2C  SDA=GPIO47, SCL=GPIO48, Addr=0x6B
    Display: SPI  via TFT_eSPI (pins in User_Setup.h)
             MOSI=41, SCLK=40, CS=39, DC=38, RST=42
    Green:   GPIO9  (Start / Confirm)
    Red:     GPIO13 (Stop / Scroll Up / Deep Sleep)
    Yellow:  GPIO10 (Calibrate / Scroll Down)

  Libraries needed (Arduino Library Manager):
    1. "TFT_eSPI" by Bodmer
    2. "ArduinoJson" by Benoit Blanchon (v7)

  Serial Commands:
    s           = Start set
    x           = Stop set
    c           = Calibrate IMU (hold still ~1 second)
    e:<name>    = Change exercise (e.g. e:Bench Press)
    p:<index>   = Select player by roster index (e.g. p:0)

  Buttons (state-dependent — see handleButtons()):
    SELECTING_PLAYER:   GRN=Confirm  YEL=ScrollDown  RED=ScrollUp       RED(3s)=Sleep
    SELECTING_EXERCISE: GRN=Confirm  YEL=ScrollDown  RED=ScrollUp       RED(3s)=Sleep
    IDLE:               GRN=Start    YEL=ChgExercise YEL(1.5s)=Calibrate RED=ChgPlayer RED(3s)=Sleep
    RUNNING:            RED=StopSet  RED(3s)=blocked
    CALIBRATING:        RED(3s)=Sleep
*/

#include <Wire.h>
#include <math.h>
#include <TFT_eSPI.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ══════════════════════════════════════════════════════════════════════
//  CONFIGURATION — edit these for your deployment
// ══════════════════════════════════════════════════════════════════════
#define WIFI_SSID       "Mitchell's Z Fold7"
#define WIFI_PASS       "txid512^"
#define BACKEND_URL     "https://vbt-api.onrender.com"
#define TEAM_ID         "dc0c7646-bfc3-4443-8e66-b2bbd41d270d"
#define DEVICE_ID       "esp32-vbt-01"

// ══════════════════════════════════════════════════════════════════════
//  HARDCODED EXERCISES
// ══════════════════════════════════════════════════════════════════════
const char* EXERCISES[] = {
  "Back Squat",
  "Front Squat",
  "Bench Press",
  "Overhead Press",
  "Deadlift",
  "Power Clean",
  "Hang Clean",
  "Push Press",
  "Romanian Deadlift",
  "Trap Bar Deadlift"
};
const int NUM_EXERCISES = sizeof(EXERCISES) / sizeof(EXERCISES[0]);

// ══════════════════════════════════════════════════════════════════════
//  PLAYER ROSTER (fetched from backend)
// ══════════════════════════════════════════════════════════════════════
struct Player {
  char id[40];
  char firstName[24];
  char lastName[24];
  int  jerseyNumber;
};

Player roster[25];
int rosterCount = 0;
bool rosterLoaded = false;
bool needsRosterFetch = false;

// Global TLS client — allocated once to avoid heap fragmentation from
// repeated new/delete of ~16-40KB TLS buffers (causes WDT resets).
WiFiClientSecure tlsClient;

// ══════════════════════════════════════════════════════════════════════
//  STORED REPS (for uploading after set completes)
// ══════════════════════════════════════════════════════════════════════
struct StoredRep {
  int   repNumber;
  float meanVelocity;
  float peakVelocity;
  float romMeters;
  float concDurationMs;
  float eccDurationMs;
  float concPeakAccel;
  float eccPeakVel;
  float eccPeakAccel;
};

StoredRep storedReps[30];
int storedRepCount = 0;

// ══════════════════════════════════════════════════════════════════════
//  20Hz VELOCITY SAMPLES (for per-rep velocity curve upload)
// ══════════════════════════════════════════════════════════════════════
struct VelSample {
  uint16_t t;   // ms relative to rep start
  int16_t  v;   // velocity * 1000 (milli-m/s)
};

const int MAX_SAMPLES_PER_REP = 80;   // ~4 sec at 20Hz
const int MAX_TOTAL_SAMPLES   = 800;  // 10 reps * 80

VelSample samplePool[MAX_TOTAL_SAMPLES];  // 3.2KB flat pool
int samplePoolUsed = 0;

struct RepSampleRange { int startIdx; int count; };
RepSampleRange repSamples[30];

int sampleDecimator = 0;
unsigned long repSampleStartMs = 0;

// ══════════════════════════════════════════════════════════════════════
//  SELECTION STATE
// ══════════════════════════════════════════════════════════════════════
int selectedPlayerIdx   = -1;
int selectedExerciseIdx = 0;
int scrollOffset        = 0;
int cursorPos           = 0;
const int VISIBLE_ROWS  = 5;

// ══════════════════════════════════════════════════════════════════════
//  DISPLAY (TFT_eSPI — pins configured in User_Setup.h)
// ══════════════════════════════════════════════════════════════════════
TFT_eSPI tft = TFT_eSPI(240, 240);

#define C_GREY 0x7BEF

bool displayDirty = true;

String activeExercise = "Back Squat";

struct LastRepData {
  int    repNum;
  float  peakVel;
  float  meanVel;
  float  peakAccel;
  float  romCm;
  int    concMs;
  int    eccMs;
  int    totalMs;
} lastRep = {0, 0, 0, 0, 0, 0, 0, 0};

int lastSetReps = 0;

// ══════════════════════════════════════════════════════════════════════
//  IMU (QMI8658)
// ══════════════════════════════════════════════════════════════════════
static const int I2C_SDA = 47;
static const int I2C_SCL = 48;
static const uint8_t QMI_ADDR = 0x6B;

static const uint8_t REG_WHO_AM_I = 0x00;
static const uint8_t REG_CTRL1    = 0x02;
static const uint8_t REG_CTRL2    = 0x03;
static const uint8_t REG_CTRL3    = 0x04;
static const uint8_t REG_CTRL5    = 0x06;
static const uint8_t REG_CTRL7    = 0x08;
static const uint8_t REG_AX_L     = 0x35;

static const float G_CONST = 9.80665f;
static const float ACC_LSB_PER_G = 4096.0f;
static const float GYRO_LSB_PER_DPS = 32.0f;

// ══════════════════════════════════════════════════════════════════════
//  I2C HELPERS
// ══════════════════════════════════════════════════════════════════════
static inline int16_t le16(const uint8_t *p) {
  return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(QMI_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool readReg(uint8_t reg, uint8_t &val) {
  Wire.beginTransmission(QMI_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)QMI_ADDR, 1) != 1) return false;
  val = Wire.read();
  return true;
}

bool readBytes(uint8_t startReg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(QMI_ADDR);
  Wire.write(startReg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)QMI_ADDR, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

void setupQMI8658() {
  writeReg(REG_CTRL1, 0x60);
  writeReg(REG_CTRL2, 0x23);
  writeReg(REG_CTRL3, 0x63);
  writeReg(REG_CTRL5, 0x11);
  writeReg(REG_CTRL7, 0x03);
  delay(50);
}

bool readIMU(float &ax, float &ay, float &az, float &gx, float &gy, float &gz) {
  uint8_t buf[12];
  if (!readBytes(REG_AX_L, buf, sizeof(buf))) return false;
  ax = (le16(&buf[0])  / ACC_LSB_PER_G) * G_CONST;
  ay = (le16(&buf[2])  / ACC_LSB_PER_G) * G_CONST;
  az = (le16(&buf[4])  / ACC_LSB_PER_G) * G_CONST;
  gx = le16(&buf[6])  / GYRO_LSB_PER_DPS;
  gy = le16(&buf[8])  / GYRO_LSB_PER_DPS;
  gz = le16(&buf[10]) / GYRO_LSB_PER_DPS;
  return true;
}

// ══════════════════════════════════════════════════════════════════════
//  DEVICE STATE (declared early so buttons can reference it)
// ══════════════════════════════════════════════════════════════════════
enum DeviceState { BOOTING, SELECTING_PLAYER, SELECTING_EXERCISE, IDLE, RUNNING, CALIBRATING };
DeviceState state = BOOTING;

// ══════════════════════════════════════════════════════════════════════
//  BUTTONS
// ══════════════════════════════════════════════════════════════════════
static const int BTN_GREEN  = 9;
static const int BTN_RED    = 13;
static const int BTN_YELLOW = 10;

static const unsigned long BTN_DEBOUNCE_MS       = 50;
static const unsigned long RED_LONG_PRESS_MS     = 3000;
static const unsigned long YELLOW_LONG_PRESS_MS  = 1500;

bool btnGreenState = false, btnRedState = false, btnYellowState = false;
unsigned long btnGreenLastChange = 0, btnRedLastChange = 0, btnYellowLastChange = 0;
unsigned long redPressStart = 0;
bool redLongFired = false;
unsigned long yellowPressStart = 0;
bool yellowLongFired = false;

// Forward declarations
void startSet();
void stopSet();
void startCalibration();
void enterDeepSleep();
void scrollDown(int totalItems);
void scrollUp();
void resetScroll();

void setupButtons() {
  pinMode(BTN_GREEN,  INPUT_PULLUP);
  pinMode(BTN_RED,    INPUT_PULLUP);
  pinMode(BTN_YELLOW, INPUT_PULLUP);
  Serial.println("BTNS: Configured (9=Green, 13=Red, 10=Yellow) INPUT_PULLUP");
}

void handleButtons() {
  unsigned long now = millis();

  // Read raw (LOW = pressed with INPUT_PULLUP)
  bool greenNow  = (digitalRead(BTN_GREEN)  == LOW);
  bool redNow    = (digitalRead(BTN_RED)    == LOW);
  bool yellowNow = (digitalRead(BTN_YELLOW) == LOW);

  // ── Green button ──
  if (greenNow != btnGreenState && (now - btnGreenLastChange) > BTN_DEBOUNCE_MS) {
    btnGreenState = greenNow;
    btnGreenLastChange = now;
    if (greenNow) {
      switch (state) {
        case SELECTING_PLAYER: {
          int idx = scrollOffset + cursorPos;
          if (idx < rosterCount) {
            selectedPlayerIdx = idx;
            activeExercise = EXERCISES[selectedExerciseIdx];
            Serial.print("Selected player: ");
            Serial.print(roster[idx].firstName);
            Serial.print(" ");
            Serial.println(roster[idx].lastName);
            resetScroll();
            state = SELECTING_EXERCISE;
            displayDirty = true;
          }
          break;
        }
        case SELECTING_EXERCISE: {
          int idx = scrollOffset + cursorPos;
          if (idx < NUM_EXERCISES) {
            selectedExerciseIdx = idx;
            activeExercise = EXERCISES[selectedExerciseIdx];
            Serial.print("Selected exercise: ");
            Serial.println(activeExercise);
            resetScroll();
            state = IDLE;
            displayDirty = true;
          }
          break;
        }
        case IDLE:
          startSet();
          break;
        default:
          break;
      }
    }
  }

  // ── Yellow button — short press + long hold ──
  if (yellowNow != btnYellowState && (now - btnYellowLastChange) > BTN_DEBOUNCE_MS) {
    btnYellowState = yellowNow;
    btnYellowLastChange = now;
    if (yellowNow) {
      // Pressed
      yellowPressStart = now;
      yellowLongFired = false;
      // Immediate action for selection screens (scrolling)
      switch (state) {
        case SELECTING_PLAYER:
          scrollDown(rosterCount);
          displayDirty = true;
          break;
        case SELECTING_EXERCISE:
          scrollDown(NUM_EXERCISES);
          displayDirty = true;
          break;
        default:
          break;
      }
    } else {
      // Released — short press action for IDLE (change exercise)
      if (!yellowLongFired && state == IDLE) {
        resetScroll();
        state = SELECTING_EXERCISE;
        displayDirty = true;
      }
    }
  }

  // Check for yellow long press while held (calibrate at IDLE)
  if (btnYellowState && !yellowLongFired && (now - yellowPressStart) >= YELLOW_LONG_PRESS_MS) {
    yellowLongFired = true;
    if (state == IDLE) {
      startCalibration();
    }
  }

  // ── Red button — short press + long hold ──
  if (redNow != btnRedState && (now - btnRedLastChange) > BTN_DEBOUNCE_MS) {
    btnRedState = redNow;
    btnRedLastChange = now;
    if (redNow) {
      // Pressed
      redPressStart = now;
      redLongFired = false;
    } else {
      // Released — action depends on state
      if (!redLongFired) {
        switch (state) {
          case SELECTING_PLAYER:
            scrollUp();
            displayDirty = true;
            break;
          case SELECTING_EXERCISE:
            scrollUp();
            displayDirty = true;
            break;
          case IDLE:
            // Change player
            resetScroll();
            state = SELECTING_PLAYER;
            displayDirty = true;
            break;
          case RUNNING:
            stopSet();
            break;
          default:
            break;
        }
      }
    }
  }

  // Check for long press while held
  if (btnRedState && !redLongFired && (now - redPressStart) >= RED_LONG_PRESS_MS) {
    redLongFired = true;
    if (state != RUNNING) {
      enterDeepSleep();
    }
  }
}

// ══════════════════════════════════════════════════════════════════════
//  SCROLL / CURSOR HELPERS
// ══════════════════════════════════════════════════════════════════════
void scrollDown(int totalItems) {
  int currentIdx = scrollOffset + cursorPos;
  if (currentIdx + 1 >= totalItems) return;  // at bottom

  if (cursorPos < VISIBLE_ROWS - 1) {
    cursorPos++;
  } else {
    scrollOffset++;
  }
}

void scrollUp() {
  if (cursorPos > 0) {
    cursorPos--;
  } else if (scrollOffset > 0) {
    scrollOffset--;
  }
}

void resetScroll() {
  scrollOffset = 0;
  cursorPos = 0;
}

// ══════════════════════════════════════════════════════════════════════
//  VBT STATE
// ══════════════════════════════════════════════════════════════════════
float gex = 0, gey = 0, gez = G_CONST;
float biasGx = 0, biasGy = 0, biasGz = 0;
bool calibrated = false;

float vVert = 0;
float dVert = 0;
unsigned long lastMicros = 0;

const float COMP_ALPHA     = 0.04f;
const float ACC_DEADBAND   = 0.12f;
const float VEL_DECAY      = 0.996f;
const float VEL_CLAMP      = 6.0f;

const float ZUPT_ACCEL_THRESH = 0.25f;
const float ZUPT_GYRO_THRESH  = 15.0f;
const int   ZUPT_COUNT_THRESH = 40;
int zuptCounter = 0;

enum Phase { REST = 0, CONCENTRIC = 1, ECCENTRIC = 2 };
Phase phase = REST;
int repCount = 0;

const float CONC_START_THRESH  = 0.10f;
const float CONC_END_THRESH    = 0.05f;
const float ECC_MOVE_THRESH    = -0.06f;
const float ECC_END_THRESH     = 0.03f;
const float MIN_CONC_MS        = 100.0f;
const float MIN_ECC_MS         = 100.0f;
const float MIN_REST_MS        = 120.0f;
const float MIN_ROM_CM         = 3.0f;

unsigned long concStartMs = 0;
unsigned long concEndMs   = 0;
float concPeakVel   = 0;
float concSumVel    = 0;
unsigned long concSamples = 0;
float concPeakAccel = 0;
float concDisplacement = 0;

unsigned long eccStartMs = 0;
float eccPeakVel    = 0;
float eccPeakAccel  = 0;
float eccSumVel     = 0;
unsigned long eccSamples = 0;
float eccDisplacement = 0;

unsigned long lastRepEndMs = 0;

float repAccelMin = 999, repAccelMax = 0, repAccelSum = 0;
float repGyroMin  = 999, repGyroMax  = 0, repGyroSum  = 0;
unsigned long repTotalSamples = 0;

const int SMA_SIZE = 5;
float smaBuffer[SMA_SIZE];
int smaIndex = 0;
bool smaFull = false;

float smaFilter(float val) {
  smaBuffer[smaIndex] = val;
  smaIndex = (smaIndex + 1) % SMA_SIZE;
  if (!smaFull && smaIndex == 0) smaFull = true;
  int count = smaFull ? SMA_SIZE : smaIndex;
  if (count == 0) return val;
  float sum = 0;
  for (int i = 0; i < count; i++) sum += smaBuffer[i];
  return sum / (float)count;
}

// ══════════════════════════════════════════════════════════════════════
//  WiFi + HTTP FUNCTIONS
// ══════════════════════════════════════════════════════════════════════
bool connectWiFi() {
  Serial.print("WiFi: Connecting to ");
  Serial.println(WIFI_SSID);

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);
  tft.setCursor(12, 80);
  tft.println("Connecting");
  tft.setCursor(12, 105);
  tft.println("to WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi: Connected! IP=");
    Serial.println(WiFi.localIP());
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN);
    tft.setCursor(12, 100);
    tft.println("WiFi OK!");
    delay(500);
    return true;
  } else {
    Serial.println("WiFi: Connection FAILED");
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED);
    tft.setCursor(12, 100);
    tft.println("WiFi FAILED");
    delay(1000);
    return false;
  }
}

bool fetchRoster() {
  Serial.println("HTTP: Fetching roster...");

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(12, 100);
  tft.println("Loading");
  tft.setCursor(12, 125);
  tft.println("roster...");
  tlsClient.stop();  // Force fresh connection (stale sockets cause failures)

  HTTPClient http;
  http.setTimeout(15000);  // 15s — Render cold starts can be slow
  String url = String(BACKEND_URL) + "/device/roster/" + TEAM_ID;
  Serial.print("HTTP: GET ");
  Serial.println(url);

  if (!http.begin(tlsClient, url)) {
    Serial.println("HTTP: begin() failed");
    return false;
  }

  int httpCode = http.GET();

  Serial.print("HTTP: Response code=");
  Serial.println(httpCode);

  if (httpCode != 200) {
    Serial.print("HTTP: Roster fetch failed, code=");
    Serial.println(httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  // Parse JSON roster
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("JSON: Parse failed: ");
    Serial.println(err.c_str());
    return false;
  }

  JsonArray arr = doc.as<JsonArray>();
  rosterCount = 0;
  for (JsonObject player : arr) {
    if (rosterCount >= 25) break;
    strlcpy(roster[rosterCount].id, player["id"] | "", sizeof(roster[0].id));
    strlcpy(roster[rosterCount].firstName, player["first_name"] | "", sizeof(roster[0].firstName));
    strlcpy(roster[rosterCount].lastName, player["last_name"] | "", sizeof(roster[0].lastName));
    roster[rosterCount].jerseyNumber = player["jersey_number"] | 0;
    rosterCount++;
  }

  rosterLoaded = (rosterCount > 0);
  Serial.print("HTTP: Loaded ");
  Serial.print(rosterCount);
  Serial.println(" players");

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN);
  tft.setCursor(12, 100);
  tft.print("Loaded ");
  tft.print(rosterCount);
  tft.println(" players");
  delay(500);

  return rosterLoaded;
}

void postSetData() {
  if (storedRepCount == 0) {
    Serial.println("HTTP: No reps to upload");
    return;
  }

  if (selectedPlayerIdx < 0 || selectedPlayerIdx >= rosterCount) {
    Serial.println("HTTP: No player selected, skipping upload");
    return;
  }

  Serial.println("HTTP: Uploading set data...");

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(12, 100);
  tft.println("Uploading");
  tft.setCursor(12, 125);
  tft.println("set...");

  // Build JSON
  JsonDocument doc;
  doc["team_id"] = TEAM_ID;
  doc["player_id"] = roster[selectedPlayerIdx].id;
  doc["exercise"] = activeExercise;
  doc["device_id"] = DEVICE_ID;

  JsonArray repsArr = doc["reps"].to<JsonArray>();
  for (int i = 0; i < storedRepCount; i++) {
    JsonObject rep = repsArr.add<JsonObject>();
    rep["rep_number"] = storedReps[i].repNumber;
    rep["mean_velocity"] = round(storedReps[i].meanVelocity * 1000.0f) / 1000.0f;
    rep["peak_velocity"] = round(storedReps[i].peakVelocity * 1000.0f) / 1000.0f;
    rep["rom_meters"] = round(storedReps[i].romMeters * 1000.0f) / 1000.0f;
    rep["concentric_duration"] = storedReps[i].concDurationMs;
    rep["eccentric_duration"] = storedReps[i].eccDurationMs;
    rep["conc_peak_accel"] = round(storedReps[i].concPeakAccel * 1000.0f) / 1000.0f;
    rep["ecc_peak_velocity"] = round(storedReps[i].eccPeakVel * 1000.0f) / 1000.0f;
    rep["ecc_peak_accel"] = round(storedReps[i].eccPeakAccel * 1000.0f) / 1000.0f;

    // 20Hz velocity samples for this rep
    JsonArray samplesArr = rep["samples"].to<JsonArray>();
    int si = repSamples[i].startIdx;
    int sc = repSamples[i].count;
    for (int j = 0; j < sc; j++) {
      JsonObject s = samplesArr.add<JsonObject>();
      s["t"] = samplePool[si + j].t;
      s["v"] = samplePool[si + j].v / 1000.0f;
    }
  }

  String body;
  serializeJson(doc, body);

  Serial.print("HTTP: POST body size=");
  Serial.println(body.length());

  String url = String(BACKEND_URL) + "/device/sets";
  int httpCode = -1;

  // Try up to 2 times (stale connection can fail on first attempt)
  for (int attempt = 0; attempt < 2; attempt++) {
    tlsClient.stop();  // Force fresh connection

    HTTPClient http;
    http.setTimeout(15000);

    if (!http.begin(tlsClient, url)) {
      Serial.println("HTTP: begin() failed");
      continue;
    }

    http.addHeader("Content-Type", "application/json");
    httpCode = http.POST(body);

    Serial.print("HTTP: Response code=");
    Serial.print(httpCode);
    Serial.print(" (attempt ");
    Serial.print(attempt + 1);
    Serial.println(")");

    if (httpCode == 201) {
      String resp = http.getString();
      Serial.print("HTTP: Success: ");
      Serial.println(resp);
      http.end();
      break;
    }

    String resp = http.getString();
    Serial.print("HTTP: Upload failed: ");
    Serial.println(resp);
    http.end();

    if (attempt == 0) {
      Serial.println("HTTP: Retrying...");
      delay(500);
    }
  }

  if (httpCode == 201) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN);
    tft.setTextSize(2);
    tft.setCursor(12, 90);
    tft.println("Uploaded OK!");
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(12, 120);
    tft.print(storedRepCount);
    tft.println(" reps saved");
    delay(1500);
  } else {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED);
    tft.setTextSize(2);
    tft.setCursor(12, 90);
    tft.println("Upload FAILED");
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    tft.setCursor(12, 120);
    tft.print("Code: ");
    tft.println(httpCode);
    delay(2000);
  }
}

// ══════════════════════════════════════════════════════════════════════
//  DISPLAY RENDERING
// ══════════════════════════════════════════════════════════════════════
void setupDisplay() {
  tft.begin();
  tft.setRotation(2);
  tft.fillScreen(TFT_BLACK);
}

void drawSelectionScreen(const char* title, int totalItems, int scrollOff, int cursor) {
  tft.fillScreen(TFT_BLACK);

  // Title bar
  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);
  tft.setCursor(6, 6);
  tft.println(title);
  tft.drawFastHLine(0, 28, 240, C_GREY);

  // List items
  int y = 34;
  int rowH = 32;
  int visible = min(VISIBLE_ROWS, totalItems - scrollOff);

  for (int i = 0; i < visible; i++) {
    int idx = scrollOff + i;
    bool highlighted = (i == cursor);

    if (highlighted) {
      tft.fillRect(0, y, 240, rowH, TFT_BLUE);
      tft.setTextColor(TFT_WHITE);
    } else {
      tft.setTextColor(TFT_WHITE);
    }

    tft.setTextSize(2);
    tft.setCursor(6, y + 8);

    if (state == SELECTING_PLAYER && idx < rosterCount) {
      // Show "#XX FirstName L."
      tft.print("#");
      if (roster[idx].jerseyNumber < 10) tft.print("0");
      tft.print(roster[idx].jerseyNumber);
      tft.print(" ");
      tft.print(roster[idx].firstName);
      tft.print(" ");
      tft.print(roster[idx].lastName[0]);
      tft.print(".");
    } else if (state == SELECTING_EXERCISE && idx < NUM_EXERCISES) {
      tft.print(EXERCISES[idx]);
    }

    y += rowH;
  }

  // Scroll indicator
  if (totalItems > VISIBLE_ROWS) {
    tft.setTextColor(C_GREY);
    tft.setTextSize(1);
    tft.setCursor(200, 8);
    tft.print(scrollOff + cursor + 1);
    tft.print("/");
    tft.print(totalItems);
  }

  // Button hints
  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(6, 210);
  tft.println("GRN=Select YEL=Down RED=Up");
  tft.setCursor(6, 225);
  tft.println("RED hold=Sleep");
}

void updateDisplay() {
  if (!displayDirty) return;
  displayDirty = false;

  if (state == SELECTING_PLAYER) {
    drawSelectionScreen("Select Player", rosterCount, scrollOffset, cursorPos);
    return;
  }

  if (state == SELECTING_EXERCISE) {
    drawSelectionScreen("Select Exercise", NUM_EXERCISES, scrollOffset, cursorPos);
    return;
  }

  if (state == BOOTING) {
    // Handled by setup flow — don't redraw
    return;
  }

  tft.fillScreen(TFT_BLACK);

  if (state == IDLE) {
    tft.setTextColor(TFT_GREEN);
    tft.setTextSize(3);
    tft.setCursor(72, 10);
    tft.println("READY");

    // Exercise
    tft.setTextColor(TFT_CYAN);
    tft.setTextSize(1);
    tft.setCursor(6, 42);
    tft.print("Exercise: ");
    tft.setTextColor(TFT_WHITE);
    tft.print(activeExercise);

    // Player info
    tft.setTextSize(1);
    tft.setCursor(6, 56);
    if (selectedPlayerIdx >= 0 && selectedPlayerIdx < rosterCount) {
      tft.setTextColor(TFT_GREEN);
      tft.print("Player: ");
      tft.setTextColor(TFT_WHITE);
      tft.print("#");
      tft.print(roster[selectedPlayerIdx].jerseyNumber);
      tft.print(" ");
      tft.print(roster[selectedPlayerIdx].firstName);
      tft.print(" ");
      tft.print(roster[selectedPlayerIdx].lastName);
    } else {
      tft.setTextColor(TFT_YELLOW);
      tft.print("No player selected");
    }

    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(2);
    if (lastSetReps > 0) {
      tft.setCursor(24, 72);
      tft.print("Last set: ");
      tft.print(lastSetReps);
      tft.println(" reps");
    }

    // Button hints
    tft.setTextColor(C_GREY);
    tft.setTextSize(1);
    tft.setCursor(12, 170);
    tft.println("GRN=Start  YEL=Chg Exercise");
    tft.setCursor(12, 185);
    tft.println("YEL hold=Calibrate");
    tft.setCursor(12, 200);
    tft.println("RED=Chg Player");
    tft.setCursor(12, 215);
    tft.println("RED hold=Sleep");
    if (calibrated) {
      tft.setTextColor(TFT_GREEN);
      tft.setCursor(160, 215);
      tft.println("CAL OK");
    }
    return;
  }

  if (state == CALIBRATING) {
    tft.setTextColor(TFT_YELLOW);
    tft.setTextSize(2);
    tft.setCursor(18, 90);
    tft.println("Calibrating...");
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(30, 130);
    tft.println("Hold still!");
    return;
  }

  // ── RUNNING state ──
  tft.setTextColor(TFT_GREEN);
  tft.setTextSize(4);
  tft.setCursor(24, 4);
  tft.print("Rep ");
  tft.println(repCount);

  tft.drawFastHLine(10, 40, 220, C_GREY);

  if (lastRep.repNum == 0) {
    tft.setTextColor(TFT_YELLOW);
    tft.setTextSize(2);
    tft.setCursor(48, 110);
    tft.println("Waiting...");
    return;
  }

  int y = 50;
  int dy = 25;
  tft.setTextSize(2);

  tft.setTextColor(TFT_CYAN); tft.setCursor(6, y);
  tft.print("Peak V "); tft.setTextColor(TFT_WHITE);
  tft.print(lastRep.peakVel, 3); tft.println(" m/s");
  y += dy;

  tft.setTextColor(TFT_CYAN); tft.setCursor(6, y);
  tft.print("Mean V "); tft.setTextColor(TFT_WHITE);
  tft.print(lastRep.meanVel, 3); tft.println(" m/s");
  y += dy;

  tft.setTextColor(TFT_CYAN); tft.setCursor(6, y);
  tft.print("Pk Acc "); tft.setTextColor(TFT_WHITE);
  tft.print(lastRep.peakAccel, 1); tft.println(" m/s2");
  y += dy;

  tft.setTextColor(TFT_CYAN); tft.setCursor(6, y);
  tft.print("ROM    "); tft.setTextColor(TFT_WHITE);
  tft.print(lastRep.romCm, 1); tft.println(" cm");
  y += dy;

  tft.setTextColor(TFT_CYAN); tft.setCursor(6, y);
  tft.print("Conc   "); tft.setTextColor(TFT_WHITE);
  tft.print(lastRep.concMs); tft.println(" ms");
  y += dy;

  tft.setTextColor(TFT_CYAN); tft.setCursor(6, y);
  tft.print("Ecc    "); tft.setTextColor(TFT_WHITE);
  tft.print(lastRep.eccMs); tft.println(" ms");
  y += dy;

  tft.setTextColor(TFT_CYAN); tft.setCursor(6, y);
  tft.print("Total  "); tft.setTextColor(TFT_WHITE);
  tft.print(lastRep.totalMs); tft.println(" ms");
}

// ══════════════════════════════════════════════════════════════════════
//  CALIBRATION
// ══════════════════════════════════════════════════════════════════════
static const int CAL_SAMPLES = 200;
int calCount = 0;
float calSumAx, calSumAy, calSumAz;
float calSumGx, calSumGy, calSumGz;

void startCalibration() {
  state = CALIBRATING;
  calCount = 0;
  calSumAx = calSumAy = calSumAz = 0;
  calSumGx = calSumGy = calSumGz = 0;
  Serial.println("CALIBRATING... Hold device completely still for ~1 second.");
  displayDirty = true;
}

void processCalibrationSample(float ax, float ay, float az, float gx, float gy, float gz) {
  calSumAx += ax; calSumAy += ay; calSumAz += az;
  calSumGx += gx; calSumGy += gy; calSumGz += gz;
  calCount++;

  if (calCount >= CAL_SAMPLES) {
    gex = calSumAx / CAL_SAMPLES;
    gey = calSumAy / CAL_SAMPLES;
    gez = calSumAz / CAL_SAMPLES;
    float mag = sqrtf(gex*gex + gey*gey + gez*gez);

    biasGx = calSumGx / CAL_SAMPLES;
    biasGy = calSumGy / CAL_SAMPLES;
    biasGz = calSumGz / CAL_SAMPLES;

    calibrated = true;

    Serial.print("CAL_DONE,grav_mag=");
    Serial.print(mag, 3);
    Serial.print(",gravDir=(");
    Serial.print(gex/mag, 3); Serial.print(",");
    Serial.print(gey/mag, 3); Serial.print(",");
    Serial.print(gez/mag, 3); Serial.print(")");
    Serial.print(",gyroBias=(");
    Serial.print(biasGx, 2); Serial.print(",");
    Serial.print(biasGy, 2); Serial.print(",");
    Serial.print(biasGz, 2); Serial.println(")");
    Serial.println("READY");
    state = IDLE;
    displayDirty = true;
  }
}

// ══════════════════════════════════════════════════════════════════════
//  PIPELINE
// ══════════════════════════════════════════════════════════════════════
void resetRepMetrics() {
  concPeakVel = 0;
  concSumVel  = 0;
  concSamples = 0;
  concPeakAccel = 0;
  concDisplacement = 0;
  eccPeakVel  = 0;
  eccPeakAccel = 0;
  eccSumVel   = 0;
  eccSamples  = 0;
  eccDisplacement = 0;
  dVert = 0;
  repAccelMin = 999; repAccelMax = 0; repAccelSum = 0;
  repGyroMin  = 999; repGyroMax  = 0; repGyroSum  = 0;
  repTotalSamples = 0;
}

void resetPipeline() {
  vVert = 0;
  dVert = 0;
  phase = REST;
  repCount = 0;
  zuptCounter = 0;
  lastRepEndMs = 0;
  smaIndex = 0;
  smaFull = false;
  for (int i = 0; i < SMA_SIZE; i++) smaBuffer[i] = 0;
  resetRepMetrics();
  lastRep.repNum = 0;
  lastMicros = micros();
}

void startSet() {
  if (state != IDLE) return;

  // v6: Use player selection instead of RFID gate
  if (selectedPlayerIdx < 0) {
    Serial.println("ERROR: Select a player before starting set.");
    tft.fillRect(0, 100, 240, 30, TFT_BLACK);
    tft.setTextColor(TFT_RED);
    tft.setTextSize(2);
    tft.setCursor(6, 105);
    tft.print("Select player!");
    return;
  }

  if (!calibrated) {
    Serial.println("WARNING: Not calibrated. Press Yellow or 'c' first for best results.");
  }
  state = RUNNING;
  resetPipeline();
  storedRepCount = 0;
  samplePoolUsed = 0;
  Serial.println("STARTED_SET");
  Serial.print("  Player: ");
  Serial.print(roster[selectedPlayerIdx].firstName);
  Serial.print(" ");
  Serial.print(roster[selectedPlayerIdx].lastName);
  Serial.print(" (");
  Serial.print(roster[selectedPlayerIdx].id);
  Serial.println(")");
  Serial.print("  Exercise: "); Serial.println(activeExercise);
  Serial.println("t_ms,aVert,vVert,dVert,ax,ay,az,gx,gy,gz,rep,phase");
  displayDirty = true;
}

void stopSet() {
  if (state != RUNNING) return;
  state = IDLE;
  lastSetReps = repCount;
  Serial.print("STOPPED_SET,total_reps=");
  Serial.println(repCount);

  // Upload set data to backend
  postSetData();

  displayDirty = true;
}

void emitRepSummary() {
  float concMeanVel = (concSamples > 0) ? (concSumVel / (float)concSamples) : 0;
  float concDurMs = concEndMs - concStartMs;
  float eccDurMs  = millis() - eccStartMs;
  float totalDurMs = concDurMs + eccDurMs;
  float romCm = fabs(concDisplacement) * 100.0f;

  if (romCm < MIN_ROM_CM) return;

  repCount++;

  // Store rep data for upload
  if (storedRepCount < 30) {
    // Finalize sample range for this rep
    repSamples[storedRepCount].count = samplePoolUsed - repSamples[storedRepCount].startIdx;

    storedReps[storedRepCount].repNumber       = repCount;
    storedReps[storedRepCount].meanVelocity    = concMeanVel;
    storedReps[storedRepCount].peakVelocity    = concPeakVel;
    storedReps[storedRepCount].romMeters       = romCm / 100.0f;
    storedReps[storedRepCount].concDurationMs  = concDurMs;
    storedReps[storedRepCount].eccDurationMs   = eccDurMs;
    storedReps[storedRepCount].concPeakAccel   = concPeakAccel;
    storedReps[storedRepCount].eccPeakVel      = fabs(eccPeakVel);
    storedReps[storedRepCount].eccPeakAccel    = eccPeakAccel;
    storedRepCount++;
  }

  Serial.print("REP_SUMMARY,");
  Serial.print(repCount);          Serial.print(",");
  Serial.print(concPeakVel, 3);    Serial.print(",");
  Serial.print(concMeanVel, 3);    Serial.print(",");
  Serial.print(concPeakAccel, 3);  Serial.print(",");
  Serial.print(romCm, 1);          Serial.print(",");
  Serial.print((int)concDurMs);    Serial.print(",");
  Serial.print((int)eccDurMs);     Serial.print(",");
  Serial.println((int)totalDurMs);

  float repAccelMean = (repTotalSamples > 0) ? (repAccelSum / (float)repTotalSamples) : 0;
  float repGyroMean  = (repTotalSamples > 0) ? (repGyroSum  / (float)repTotalSamples) : 0;

  Serial.print("REP_SENSORS,");
  Serial.print(repCount);         Serial.print(",");
  Serial.print(repAccelMin, 3);   Serial.print(",");
  Serial.print(repAccelMax, 3);   Serial.print(",");
  Serial.print(repAccelMean, 3);  Serial.print(",");
  Serial.print(repGyroMin, 2);    Serial.print(",");
  Serial.print(repGyroMax, 2);    Serial.print(",");
  Serial.println(repGyroMean, 2);

  // Save for display
  lastRep.repNum    = repCount;
  lastRep.peakVel   = concPeakVel;
  lastRep.meanVel   = concMeanVel;
  lastRep.peakAccel = concPeakAccel;
  lastRep.romCm     = romCm;
  lastRep.concMs    = (int)concDurMs;
  lastRep.eccMs     = (int)eccDurMs;
  lastRep.totalMs   = (int)totalDurMs;

  displayDirty = true;
}

// ══════════════════════════════════════════════════════════════════════
//  MAIN VBT PROCESSING
// ══════════════════════════════════════════════════════════════════════
void processVBT(float ax, float ay, float az, float gx_raw, float gy_raw, float gz_raw) {
  float gx = gx_raw - biasGx;
  float gy = gy_raw - biasGy;
  float gz = gz_raw - biasGz;

  unsigned long now = micros();
  float dt = (now - lastMicros) * 1e-6f;
  if (dt <= 0 || dt > 0.1f) dt = 0.005f;
  lastMicros = now;

  float gx_rad = gx * DEG_TO_RAD;
  float gy_rad = gy * DEG_TO_RAD;
  float gz_rad = gz * DEG_TO_RAD;

  float new_gex = gex + (gey * gz_rad - gez * gy_rad) * dt;
  float new_gey = gey + (gez * gx_rad - gex * gz_rad) * dt;
  float new_gez = gez + (gex * gy_rad - gey * gx_rad) * dt;

  gex = (1.0f - COMP_ALPHA) * new_gex + COMP_ALPHA * ax;
  gey = (1.0f - COMP_ALPHA) * new_gey + COMP_ALPHA * ay;
  gez = (1.0f - COMP_ALPHA) * new_gez + COMP_ALPHA * az;

  float gmag = sqrtf(gex*gex + gey*gey + gez*gez);
  if (gmag < 0.1f) gmag = G_CONST;
  float ugx = gex / gmag;
  float ugy = gey / gmag;
  float ugz = gez / gmag;

  float aAlongG = ax*ugx + ay*ugy + az*ugz;
  float aVert = aAlongG - G_CONST;

  aVert = smaFilter(aVert);
  if (fabs(aVert) < ACC_DEADBAND) aVert = 0;

  float gyroMag = sqrtf(gx*gx + gy*gy + gz*gz);

  if (fabs(aVert) < ZUPT_ACCEL_THRESH && gyroMag < ZUPT_GYRO_THRESH) {
    zuptCounter++;
  } else {
    zuptCounter = 0;
  }
  bool isStationary = (zuptCounter >= ZUPT_COUNT_THRESH);

  if (isStationary) {
    vVert *= 0.90f;
  } else {
    vVert += aVert * dt;
    vVert *= VEL_DECAY;
  }

  if (vVert >  VEL_CLAMP) vVert =  VEL_CLAMP;
  if (vVert < -VEL_CLAMP) vVert = -VEL_CLAMP;

  if (phase != REST) {
    dVert += vVert * dt;
  }

  float accelMag = sqrtf(ax*ax + ay*ay + az*az);
  if (phase != REST) {
    if (accelMag < repAccelMin) repAccelMin = accelMag;
    if (accelMag > repAccelMax) repAccelMax = accelMag;
    repAccelSum += accelMag;
    if (gyroMag < repGyroMin) repGyroMin = gyroMag;
    if (gyroMag > repGyroMax) repGyroMax = gyroMag;
    repGyroSum += gyroMag;
    repTotalSamples++;
  }

  unsigned long tms = millis();

  // Capture 20Hz velocity samples during active rep phases
  if (phase != REST) {
    sampleDecimator++;
    if (sampleDecimator >= 10 && samplePoolUsed < MAX_TOTAL_SAMPLES) {
      sampleDecimator = 0;
      samplePool[samplePoolUsed].t = (uint16_t)(tms - repSampleStartMs);
      samplePool[samplePoolUsed].v = (int16_t)(vVert * 1000.0f);
      samplePoolUsed++;
    }
  }

  switch (phase) {
    case REST: {
      if ((tms - lastRepEndMs) > (unsigned long)MIN_REST_MS && vVert > CONC_START_THRESH) {
        phase = CONCENTRIC;
        concStartMs = tms;
        resetRepMetrics();
        concPeakVel = vVert;
        dVert = 0;
        // Track sample range for this rep
        repSampleStartMs = tms;
        sampleDecimator = 0;
        repSamples[storedRepCount].startIdx = samplePoolUsed;
      }
      break;
    }
    case CONCENTRIC: {
      if (vVert > concPeakVel) concPeakVel = vVert;
      if (fabs(aVert) > concPeakAccel) concPeakAccel = fabs(aVert);
      concSumVel += vVert;
      concSamples++;
      concDisplacement = dVert;
      if (vVert < CONC_END_THRESH && (tms - concStartMs) > (unsigned long)MIN_CONC_MS) {
        concEndMs = tms;
        phase = ECCENTRIC;
        eccStartMs = tms;
        eccPeakVel = 0;
      }
      if ((tms - concStartMs) > 5000) {
        phase = REST;
        lastRepEndMs = tms;
      }
      break;
    }
    case ECCENTRIC: {
      if (vVert < eccPeakVel) eccPeakVel = vVert;
      if (fabs(aVert) > eccPeakAccel) eccPeakAccel = fabs(aVert);
      eccSumVel += fabs(vVert);
      eccSamples++;
      eccDisplacement = dVert - concDisplacement;
      bool hasMovedDown = (eccPeakVel < ECC_MOVE_THRESH);
      if (hasMovedDown && fabs(vVert) < ECC_END_THRESH && (tms - eccStartMs) > (unsigned long)MIN_ECC_MS) {
        emitRepSummary();
        lastRepEndMs = tms;
        phase = REST;
      }
      if ((tms - eccStartMs) > 5000) {
        emitRepSummary();
        lastRepEndMs = tms;
        phase = REST;
      }
      break;
    }
  }

  // CSV output
  Serial.print(tms);              Serial.print(",");
  Serial.print(aVert, 4);         Serial.print(",");
  Serial.print(vVert, 4);         Serial.print(",");
  Serial.print(dVert, 4);         Serial.print(",");
  Serial.print(ax, 3);            Serial.print(",");
  Serial.print(ay, 3);            Serial.print(",");
  Serial.print(az, 3);            Serial.print(",");
  Serial.print(gx, 2);            Serial.print(",");
  Serial.print(gy, 2);            Serial.print(",");
  Serial.print(gz, 2);            Serial.print(",");
  Serial.print(repCount);         Serial.print(",");
  Serial.println((int)phase);
}

// ══════════════════════════════════════════════════════════════════════
//  NON-BLOCKING SERIAL COMMAND PARSER
// ══════════════════════════════════════════════════════════════════════
const int CMD_BUF_SIZE = 128;
char cmdBuffer[CMD_BUF_SIZE];
int cmdLen = 0;

void processCommand(const String& cmd) {
  if (cmd.length() == 0) return;

  char first = cmd.charAt(0);

  // Single-char commands
  if (cmd.length() == 1) {
    switch (first) {
      case 'c': case 'C': startCalibration(); return;
      case 's': case 'S': startSet();         return;
      case 'x': case 'X': stopSet();          return;
    }
  }

  // Multi-char commands with ':' separator
  int colonIdx = cmd.indexOf(':');
  if (colonIdx > 0) {
    String prefix = cmd.substring(0, colonIdx);
    String value  = cmd.substring(colonIdx + 1);
    value.trim();

    if (prefix == "e" || prefix == "E") {
      activeExercise = value;
      Serial.print("Exercise set to: ");
      Serial.println(activeExercise);
      displayDirty = true;
      return;
    }

    if (prefix == "p" || prefix == "P") {
      int idx = value.toInt();
      if (idx >= 0 && idx < rosterCount) {
        selectedPlayerIdx = idx;
        Serial.print("Player selected by index: ");
        Serial.print(roster[idx].firstName);
        Serial.print(" ");
        Serial.println(roster[idx].lastName);
        displayDirty = true;
      } else {
        Serial.print("Invalid player index: ");
        Serial.print(idx);
        Serial.print(" (roster has ");
        Serial.print(rosterCount);
        Serial.println(" players)");
      }
      return;
    }
  }

  Serial.print("Unknown command: ");
  Serial.println(cmd);
  Serial.println("Commands: s, x, c, e:<name>, u:<uid>, p:<index>");
}

void handleSerialInput() {
  while (Serial.available()) {
    char c = (char)Serial.read();

    if (c == '\n' || c == '\r') {
      if (cmdLen > 0) {
        cmdBuffer[cmdLen] = '\0';
        String cmd = String(cmdBuffer);
        cmd.trim();
        processCommand(cmd);
        cmdLen = 0;
      }
    } else {
      if (cmdLen < CMD_BUF_SIZE - 1) {
        cmdBuffer[cmdLen++] = c;
      }
    }
  }
}

// ══════════════════════════════════════════════════════════════════════
//  DEEP SLEEP
// ══════════════════════════════════════════════════════════════════════
void enterDeepSleep() {
  Serial.println("DEEP_SLEEP: Entering deep sleep...");

  // Show sleep message on display
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_MAGENTA);
  tft.setTextSize(2);
  tft.setCursor(36, 100);
  tft.println("Sleeping...");
  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(18, 130);
  tft.println("Press any button to wake");

  // Disconnect WiFi
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  delay(100);

  // Configure wakeup source — any button press (active LOW)
  uint64_t wakeupMask = (1ULL << BTN_GREEN) | (1ULL << BTN_RED) | (1ULL << BTN_YELLOW);
#if defined(ESP_EXT1_WAKEUP_ANY_LOW)
  // ESP-IDF 5.x+ (Arduino core 3.x): any single button wakes
  esp_sleep_enable_ext1_wakeup(wakeupMask, ESP_EXT1_WAKEUP_ANY_LOW);
#else
  // ESP-IDF 4.x (Arduino core 2.x): ext0 supports one pin, use Green
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_GREEN, 0);
#endif

  esp_deep_sleep_start();
  // Device reboots through setup() on wake
}

// ══════════════════════════════════════════════════════════════════════
//  ARDUINO SETUP & LOOP
// ══════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(800);

  Serial.println("╔═══════════════════════════════════════╗");
  Serial.println("║   VBT IMU v6 (WiFi + Buttons)         ║");
  Serial.println("║   WiFi + Player Select + DB Upload     ║");
  Serial.println("╚═══════════════════════════════════════╝");

  setupButtons();

  // IMU (I2C — separate bus entirely)
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  delay(20);

  uint8_t who = 0;
  if (!readReg(REG_WHO_AM_I, who)) {
    Serial.println("IMU: WHO_AM_I read failed.");
  } else {
    Serial.print("IMU: WHO_AM_I=0x");
    Serial.println(who, HEX);
  }
  setupQMI8658();

  setupDisplay();

  // Connect to WiFi
  bool wifiOk = connectWiFi();

  // Configure global TLS client once (avoids repeated heap alloc/free)
  tlsClient.setInsecure();                // Skip cert verification for v1

  // Fetch roster — deferred to loop() to avoid WDT reset in setup()
  needsRosterFetch = wifiOk;

  Serial.println();
  Serial.println("Commands:");
  Serial.println("  s           Start set (or Green button)");
  Serial.println("  x           Stop set (or Red button)");
  Serial.println("  c           Calibrate IMU");
  Serial.println("  e:<name>    Change exercise");
  Serial.println("  p:<index>   Select player by roster index");
  Serial.println("  Red hold 3s = Deep sleep");
  Serial.println();

  lastMicros = micros();
  displayDirty = true;
}

void loop() {
  // Deferred roster fetch — runs on first loop iteration to avoid WDT in setup()
  if (needsRosterFetch) {
    needsRosterFetch = false;
    fetchRoster();
    if (rosterLoaded && selectedPlayerIdx < 0) {
      state = SELECTING_PLAYER;
    }
    displayDirty = true;
  }

  handleSerialInput();
  handleButtons();
  updateDisplay();

  if (state == BOOTING || state == SELECTING_PLAYER || state == SELECTING_EXERCISE) {
    delay(50);
    return;
  }

  if (state == IDLE) {
    delay(10);
    return;
  }

  float ax, ay, az, gx, gy, gz;
  if (!readIMU(ax, ay, az, gx, gy, gz)) return;

  if (state == CALIBRATING) {
    processCalibrationSample(ax, ay, az, gx, gy, gz);
    delay(5);
    return;
  }

  if (state == RUNNING) {
    processVBT(ax, ay, az, gx, gy, gz);
  }

  delay(2);  // ~200Hz
}
