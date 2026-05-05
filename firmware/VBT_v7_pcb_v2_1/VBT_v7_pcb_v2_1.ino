/*
  VBT_v7_pcb_v2_1.ino
  Production firmware for VBT v2.1 PCB (XIAO ESP32-S3).

  DEVICE_ID is a compile-time #define near the top of this file. Edit it
  before flashing each unit (vbt-001, vbt-002, ...). The value is baked
  into the firmware binary — no NVS, no network needed at boot.

  End-to-end flow:
    1. Boot — init display, IMU, NFC, button ladder
    2. Show Aiken HS splash with "Press START to enter"
    3. Green press → connect WiFi → poll /device/status
       - If unpaired: show "PAIR ME: vbt-XXX"; coach pairs in web app
       - If paired:   show coach name briefly, advance to WAITING_FOR_TAG
    4. WAITING_FOR_TAG — poll PN532 for an RFID card
       - On read: GET /device/lookup?device_id=X&uid=Y
       - 0 matches: flash "Unknown tag", stay
       - 1 match:   load player, advance to IDLE
       - N matches: scroll selection screen (Yellow=down, Red=up, Green=confirm)
    5. IDLE — Green starts set, Yellow changes exercise, Yellow long calibrates,
              Red ejects player back to WAITING_FOR_TAG, Red long sleeps
    6. RUNNING — VBT pipeline; Red stops set, uploads to /device/sets,
                 returns to WAITING_FOR_TAG

  Hardware (v2.1 PCB):
    Display (GC9A01)  SPI  SCK=1, MOSI=2, CS=3, DC=4, RST=9
    IMU (QMI8658)     I2C  SDA=5, SCL=6, addr=0x6A
    PN532 NFC         SPI  shared SCK=1, MOSI=2; MISO=43, SS=44 (DIPs OFF/OFF)
    Buttons           analog ladder on GPIO7 (1k/2.2k/4.7k + 10k pulldown)

  Libraries:
    - Adafruit GC9A01A
    - Adafruit GFX Library
    - Adafruit PN532
    - Adafruit BusIO
    - ArduinoJson (v7)
*/

#include <Wire.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <Adafruit_PN532.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "logo.h"

// ══════════════════════════════════════════════════════════════════════
//  CONFIG — edit per deployment
// ══════════════════════════════════════════════════════════════════════
#define WIFI_SSID    "Mitchell's Z Fold7"
#define WIFI_PASS    "txid512^"
#define BACKEND_URL  "https://vbt-api.onrender.com"

// ⚠ EDIT BEFORE FLASHING EACH UNIT — increment per device (vbt-001, vbt-002, ...)
// Baked into the firmware binary. Survives reboots, network outages, and reflashes
// (until you flash with a different value). No NVS, no network needed at boot.
#define DEVICE_ID  "vbt-001"

// ══════════════════════════════════════════════════════════════════════
//  PIN MAP — v2.1 PCB
// ══════════════════════════════════════════════════════════════════════
// SPI shared bus
#define SPI_SCK    1
#define SPI_MOSI   2

// Display
#define TFT_CS     3
#define TFT_DC     4
#define TFT_RST    9

// IMU
#define I2C_SDA    5
#define I2C_SCL    6
#define IMU_ADDR   0x6A

// PN532
#define PN532_MISO 43
#define PN532_SS   44

// Button resistor ladder (12-bit ADC; thresholds from breadboard test)
#define BTN_PIN         7
#define BTN_GREEN_MIN   3535   // GREEN ~3720
#define BTN_RED_MIN     3070   // RED   ~3350
#define BTN_YELLOW_MIN  1400   // YELLOW ~2790; well above noise floor

#define BTN_DEBOUNCE_MS        50
#define RED_LONG_PRESS_MS      3000
#define YELLOW_LONG_PRESS_MS   1500

enum Button { BTN_NONE, BTN_GREEN, BTN_RED, BTN_YELLOW };

// ══════════════════════════════════════════════════════════════════════
//  EXERCISES (selectable via Yellow short-press)
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
//  PLAYER (current lifter, set after RFID lookup)
// ══════════════════════════════════════════════════════════════════════
struct Player {
  char id[40];
  char team_id[40];
  char first_name[24];
  char last_name[24];
  char team_name[32];
  int  jersey_number;
};

Player currentPlayer = {"", "", "", "", "", 0};
bool playerLoaded = false;

// Multi-match candidates (when one UID maps to multiple players)
const int MAX_CANDIDATES = 6;
Player candidates[MAX_CANDIDATES];
int candidateCount = 0;

// ══════════════════════════════════════════════════════════════════════
//  STORED REPS (uploaded after stopSet)
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

struct VelSample { uint16_t t; int16_t v; };
const int MAX_TOTAL_SAMPLES = 800;
VelSample samplePool[MAX_TOTAL_SAMPLES];
int samplePoolUsed = 0;
struct RepSampleRange { int startIdx; int count; };
RepSampleRange repSamples[30];
int sampleDecimator = 0;
unsigned long repSampleStartMs = 0;

// ══════════════════════════════════════════════════════════════════════
//  STATE MACHINE
// ══════════════════════════════════════════════════════════════════════
enum DeviceState {
  BOOT,
  SPLASH,
  WIFI_CONNECT,
  PAIRING,
  WAITING_FOR_TAG,
  SELECT_PLAYER,
  SELECT_EXERCISE,
  IDLE,
  RUNNING,
  CALIBRATING,
};
DeviceState state = BOOT;
String coachName = "";

// scroll cursor for SELECT_PLAYER and SELECT_EXERCISE
int scrollOffset = 0;
int cursorPos    = 0;
const int VISIBLE_ROWS = 5;

bool displayDirty = true;
String activeExercise = "Back Squat";

// ══════════════════════════════════════════════════════════════════════
//  HARDWARE OBJECTS
// ══════════════════════════════════════════════════════════════════════
Adafruit_GC9A01A tft(TFT_CS, TFT_DC, SPI_MOSI, SPI_SCK, TFT_RST);
Adafruit_PN532 nfc(SPI_SCK, PN532_MISO, SPI_MOSI, PN532_SS);
WiFiClientSecure tlsClient;

bool imuOk = false;
bool nfcOk = false;

// Display palette (using GC9A01A constants)
#define C_GREY 0x7BEF

// ══════════════════════════════════════════════════════════════════════
//  IMU (QMI8658) — same registers as v6, address 0x6A on this PCB
// ══════════════════════════════════════════════════════════════════════
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

static inline int16_t le16(const uint8_t *p) {
  return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool imuWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool imuReadReg(uint8_t reg, uint8_t &val) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)IMU_ADDR, 1) != 1) return false;
  val = Wire.read();
  return true;
}

bool imuReadBytes(uint8_t startReg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(startReg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)IMU_ADDR, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

void setupQMI8658() {
  // Soft reset
  imuWriteReg(0x60, 0xB0);
  delay(50);
  imuWriteReg(REG_CTRL1, 0x60);
  imuWriteReg(REG_CTRL2, 0x23);  // accel ±4g, ODR 250Hz
  imuWriteReg(REG_CTRL3, 0x63);  // gyro ±512dps, ODR 250Hz
  imuWriteReg(REG_CTRL5, 0x11);
  imuWriteReg(REG_CTRL7, 0x03);
  delay(50);
}

bool readIMU(float &ax, float &ay, float &az, float &gx, float &gy, float &gz) {
  uint8_t buf[12];
  if (!imuReadBytes(REG_AX_L, buf, sizeof(buf))) return false;
  ax = (le16(&buf[0])  / ACC_LSB_PER_G) * G_CONST;
  ay = (le16(&buf[2])  / ACC_LSB_PER_G) * G_CONST;
  az = (le16(&buf[4])  / ACC_LSB_PER_G) * G_CONST;
  gx = le16(&buf[6])  / GYRO_LSB_PER_DPS;
  gy = le16(&buf[8])  / GYRO_LSB_PER_DPS;
  gz = le16(&buf[10]) / GYRO_LSB_PER_DPS;
  return true;
}

// ══════════════════════════════════════════════════════════════════════
//  BUTTONS — analog ladder
// ══════════════════════════════════════════════════════════════════════
bool btnGreenState = false, btnRedState = false, btnYellowState = false;
unsigned long btnGreenLastChange = 0, btnRedLastChange = 0, btnYellowLastChange = 0;
unsigned long redPressStart = 0;
bool redLongFired = false;
unsigned long yellowPressStart = 0;
bool yellowLongFired = false;

Button readButtonRaw() {
  int v = analogRead(BTN_PIN);
  if (v >= BTN_GREEN_MIN)  return BTN_GREEN;
  if (v >= BTN_RED_MIN)    return BTN_RED;
  if (v >= BTN_YELLOW_MIN) return BTN_YELLOW;
  return BTN_NONE;
}

// Forward declarations
void startSet();
void stopSet();
void startCalibration();
void enterDeepSleep();
void scrollDown(int totalItems);
void scrollUp();
void resetScroll();
void ejectPlayer();
int  lookupTag(const String& uid);
void postSetData();

void handleButtons() {
  unsigned long now = millis();
  Button b = readButtonRaw();
  bool greenNow  = (b == BTN_GREEN);
  bool redNow    = (b == BTN_RED);
  bool yellowNow = (b == BTN_YELLOW);

  // ── Green ──
  if (greenNow != btnGreenState && (now - btnGreenLastChange) > BTN_DEBOUNCE_MS) {
    btnGreenState = greenNow;
    btnGreenLastChange = now;
    if (greenNow) {
      switch (state) {
        case SPLASH: {
          state = WIFI_CONNECT;
          displayDirty = true;
          break;
        }
        case SELECT_PLAYER: {
          int idx = scrollOffset + cursorPos;
          if (idx < candidateCount) {
            currentPlayer = candidates[idx];
            playerLoaded = true;
            Serial.print("Selected player: ");
            Serial.print(currentPlayer.first_name);
            Serial.print(" "); Serial.println(currentPlayer.last_name);
            resetScroll();
            state = IDLE;
            displayDirty = true;
          }
          break;
        }
        case SELECT_EXERCISE: {
          int idx = scrollOffset + cursorPos;
          if (idx < NUM_EXERCISES) {
            activeExercise = EXERCISES[idx];
            Serial.print("Exercise: "); Serial.println(activeExercise);
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

  // ── Yellow ──
  if (yellowNow != btnYellowState && (now - btnYellowLastChange) > BTN_DEBOUNCE_MS) {
    btnYellowState = yellowNow;
    btnYellowLastChange = now;
    if (yellowNow) {
      yellowPressStart = now;
      yellowLongFired = false;
      if (state == SELECT_PLAYER) {
        scrollDown(candidateCount); displayDirty = true;
      } else if (state == SELECT_EXERCISE) {
        scrollDown(NUM_EXERCISES); displayDirty = true;
      }
    } else {
      // released
      if (!yellowLongFired && state == IDLE) {
        resetScroll();
        state = SELECT_EXERCISE;
        displayDirty = true;
      }
    }
  }
  // Yellow long-press → calibrate (in IDLE only)
  if (btnYellowState && !yellowLongFired && (now - yellowPressStart) >= YELLOW_LONG_PRESS_MS) {
    yellowLongFired = true;
    if (state == IDLE) startCalibration();
  }

  // ── Red ──
  if (redNow != btnRedState && (now - btnRedLastChange) > BTN_DEBOUNCE_MS) {
    btnRedState = redNow;
    btnRedLastChange = now;
    if (redNow) {
      redPressStart = now;
      redLongFired = false;
    } else {
      if (!redLongFired) {
        switch (state) {
          case SELECT_PLAYER:
          case SELECT_EXERCISE:
            scrollUp(); displayDirty = true; break;
          case IDLE:
            ejectPlayer(); break;
          case RUNNING:
            stopSet(); break;
          default:
            break;
        }
      }
    }
  }
  // Red long-press → sleep (anywhere except RUNNING)
  if (btnRedState && !redLongFired && (now - redPressStart) >= RED_LONG_PRESS_MS) {
    redLongFired = true;
    if (state != RUNNING) enterDeepSleep();
  }
}

// ══════════════════════════════════════════════════════════════════════
//  SCROLL HELPERS
// ══════════════════════════════════════════════════════════════════════
void scrollDown(int total) {
  int cur = scrollOffset + cursorPos;
  if (cur + 1 >= total) return;
  if (cursorPos < VISIBLE_ROWS - 1) cursorPos++;
  else scrollOffset++;
}
void scrollUp() {
  if (cursorPos > 0) cursorPos--;
  else if (scrollOffset > 0) scrollOffset--;
}
void resetScroll() { scrollOffset = 0; cursorPos = 0; }

// ══════════════════════════════════════════════════════════════════════
//  VBT PIPELINE STATE (lifted from v6)
// ══════════════════════════════════════════════════════════════════════
float gex = 0, gey = 0, gez = G_CONST;
float biasGx = 0, biasGy = 0, biasGz = 0;
bool calibrated = false;

float vVert = 0, dVert = 0;
unsigned long lastMicros = 0;

const float COMP_ALPHA   = 0.04f;
const float ACC_DEADBAND = 0.12f;
const float VEL_DECAY    = 0.996f;
const float VEL_CLAMP    = 6.0f;

const float ZUPT_ACCEL_THRESH = 0.25f;
const float ZUPT_GYRO_THRESH  = 15.0f;
const int   ZUPT_COUNT_THRESH = 40;
int zuptCounter = 0;

enum Phase { REST = 0, CONCENTRIC = 1, ECCENTRIC = 2 };
Phase phase = REST;
int repCount = 0;

const float CONC_START_THRESH = 0.10f;
const float CONC_END_THRESH   = 0.05f;
const float ECC_MOVE_THRESH   = -0.06f;
const float ECC_END_THRESH    = 0.03f;
const float MIN_CONC_MS  = 100.0f;
const float MIN_ECC_MS   = 100.0f;
const float MIN_REST_MS  = 120.0f;
const float MIN_ROM_CM   = 3.0f;

unsigned long concStartMs = 0, concEndMs = 0;
float concPeakVel = 0, concSumVel = 0;
unsigned long concSamples = 0;
float concPeakAccel = 0, concDisplacement = 0;

unsigned long eccStartMs = 0;
float eccPeakVel = 0, eccPeakAccel = 0, eccSumVel = 0;
unsigned long eccSamples = 0;
float eccDisplacement = 0;
unsigned long lastRepEndMs = 0;

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

struct LastRepData {
  int    repNum;
  float  peakVel, meanVel, peakAccel, romCm;
  int    concMs, eccMs, totalMs;
} lastRep = {0, 0, 0, 0, 0, 0, 0, 0};
int lastSetReps = 0;

void resetRepMetrics() {
  concPeakVel = concSumVel = 0; concSamples = 0;
  concPeakAccel = concDisplacement = 0;
  eccPeakVel = eccPeakAccel = eccSumVel = 0; eccSamples = 0;
  eccDisplacement = 0; dVert = 0;
}

void resetPipeline() {
  vVert = dVert = 0;
  phase = REST;
  repCount = 0;
  zuptCounter = 0;
  lastRepEndMs = 0;
  smaIndex = 0; smaFull = false;
  for (int i = 0; i < SMA_SIZE; i++) smaBuffer[i] = 0;
  resetRepMetrics();
  lastRep.repNum = 0;
  lastMicros = micros();
}

// ══════════════════════════════════════════════════════════════════════
//  WiFi + HTTP
// ══════════════════════════════════════════════════════════════════════
bool connectWiFi() {
  Serial.print("WiFi: connecting to "); Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                     // keep radio in stable mode
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi: connected, IP="); Serial.println(WiFi.localIP());
    Serial.print("WiFi: RSSI="); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
    return true;
  }
  Serial.println("WiFi: failed");
  return false;
}

// Return true if device is paired; sets coachName.
bool checkPairingStatus() {
  tlsClient.stop();

  HTTPClient http;
  http.setTimeout(10000);
  String url = String(BACKEND_URL) + "/device/status?device_id=" + DEVICE_ID;
  if (!http.begin(tlsClient, url)) {
    Serial.println("HTTP: status begin() failed");
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.print("HTTP: status code="); Serial.println(code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  bool paired = doc["paired"] | false;
  if (paired) {
    const char* name = doc["coach_name"] | "";
    coachName = String(name);
  } else {
    coachName = "";
  }
  return paired;
}

// URL-encode a string (lightweight; covers colons + non-ASCII).
String urlEncode(const String& s) {
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

// Look up a UID. Populates `candidates` and returns count (0/1/N).
// On 1 → also fills currentPlayer; caller transitions state.
int lookupTag(const String& uid) {
  candidateCount = 0;
  tlsClient.stop();

  HTTPClient http;
  http.setTimeout(10000);
  String url = String(BACKEND_URL) + "/device/lookup?device_id=" + DEVICE_ID + "&uid=" + urlEncode(uid);
  Serial.print("HTTP: GET "); Serial.println(url);

  if (!http.begin(tlsClient, url)) {
    Serial.println("HTTP: lookup begin() failed");
    return 0;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.print("HTTP: lookup code="); Serial.println(code);
    http.end();
    return 0;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) return 0;
  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject p : arr) {
    if (candidateCount >= MAX_CANDIDATES) break;
    Player &c = candidates[candidateCount++];
    strlcpy(c.id,         p["id"]            | "", sizeof(c.id));
    strlcpy(c.team_id,    p["team_id"]       | "", sizeof(c.team_id));
    strlcpy(c.first_name, p["first_name"]    | "", sizeof(c.first_name));
    strlcpy(c.last_name,  p["last_name"]     | "", sizeof(c.last_name));
    strlcpy(c.team_name,  p["team_name"]     | "", sizeof(c.team_name));
    c.jersey_number = p["jersey_number"] | 0;
  }
  return candidateCount;
}

void postSetData() {
  if (storedRepCount == 0) { Serial.println("HTTP: no reps to upload"); return; }
  if (!playerLoaded)       { Serial.println("HTTP: no player, skipping"); return; }

  Serial.println("HTTP: uploading set...");

  JsonDocument doc;
  doc["team_id"]   = currentPlayer.team_id;
  doc["player_id"] = currentPlayer.id;
  doc["exercise"]  = activeExercise;
  doc["device_id"] = DEVICE_ID;

  JsonArray repsArr = doc["reps"].to<JsonArray>();
  for (int i = 0; i < storedRepCount; i++) {
    JsonObject rep = repsArr.add<JsonObject>();
    rep["rep_number"]          = storedReps[i].repNumber;
    rep["mean_velocity"]       = round(storedReps[i].meanVelocity * 1000.0f) / 1000.0f;
    rep["peak_velocity"]       = round(storedReps[i].peakVelocity * 1000.0f) / 1000.0f;
    rep["rom_meters"]          = round(storedReps[i].romMeters * 1000.0f) / 1000.0f;
    rep["concentric_duration"] = storedReps[i].concDurationMs;
    rep["eccentric_duration"]  = storedReps[i].eccDurationMs;
    rep["conc_peak_accel"]     = round(storedReps[i].concPeakAccel * 1000.0f) / 1000.0f;
    rep["ecc_peak_velocity"]   = round(storedReps[i].eccPeakVel * 1000.0f) / 1000.0f;
    rep["ecc_peak_accel"]      = round(storedReps[i].eccPeakAccel * 1000.0f) / 1000.0f;

    JsonArray sa = rep["samples"].to<JsonArray>();
    int si = repSamples[i].startIdx, sc = repSamples[i].count;
    for (int j = 0; j < sc; j++) {
      JsonObject s = sa.add<JsonObject>();
      s["t"] = samplePool[si + j].t;
      s["v"] = samplePool[si + j].v / 1000.0f;
    }
  }

  String body;
  serializeJson(doc, body);

  String url = String(BACKEND_URL) + "/device/sets";
  int code = -1;
  for (int attempt = 0; attempt < 2; attempt++) {
    tlsClient.stop();
    HTTPClient http;
    http.setTimeout(15000);
    if (!http.begin(tlsClient, url)) continue;
    http.addHeader("Content-Type", "application/json");
    code = http.POST(body);
    Serial.print("HTTP: POST /device/sets code="); Serial.println(code);
    if (code == 201) { http.end(); break; }
    String resp = http.getString();
    Serial.print("HTTP: error body: "); Serial.println(resp);
    http.end();
    if (attempt == 0) delay(500);
  }

  // Show result briefly
  tft.fillScreen(GC9A01A_BLACK);
  tft.setTextSize(2);
  if (code == 201) {
    tft.setTextColor(GC9A01A_GREEN);
    tft.setCursor(20, 90);
    tft.println("Uploaded!");
    tft.setTextColor(GC9A01A_WHITE);
    tft.setCursor(20, 120);
    tft.print(storedRepCount); tft.println(" reps saved");
  } else {
    tft.setTextColor(GC9A01A_RED);
    tft.setCursor(20, 90);
    tft.println("Upload FAILED");
    tft.setTextColor(GC9A01A_WHITE);
    tft.setTextSize(1);
    tft.setCursor(20, 130);
    tft.print("Code: "); tft.println(code);
  }
  delay(1500);
}

// ══════════════════════════════════════════════════════════════════════
//  DISPLAY RENDERING
// ══════════════════════════════════════════════════════════════════════
void setupDisplay() {
  tft.begin();
  tft.fillScreen(GC9A01A_BLACK);
}

void drawCenteredText(const char* s, int y, uint16_t color, int size) {
  tft.setTextColor(color);
  tft.setTextSize(size);
  int w = strlen(s) * 6 * size;
  int x = (240 - w) / 2;
  if (x < 0) x = 0;
  tft.setCursor(x, y);
  tft.print(s);
}

void drawSplashScreen() {
  tft.drawRGBBitmap(0, 0, logo_240x240, 240, 240);
  // Black bar at the bottom of the visible circle for the prompt text.
  tft.fillRect(0, 208, 240, 32, GC9A01A_BLACK);
  drawCenteredText("Press START to enter", 218, GC9A01A_WHITE, 1);
}

void drawPairingScreen() {
  tft.fillScreen(GC9A01A_BLACK);
  drawCenteredText("PAIR ME", 40, GC9A01A_CYAN, 2);

  tft.setTextColor(GC9A01A_WHITE);
  tft.setTextSize(2);
  // Center the device id
  int w = strlen(DEVICE_ID) * 12;
  int x = (240 - w) / 2; if (x < 0) x = 0;
  tft.setCursor(x, 90);
  tft.print(DEVICE_ID);

  drawCenteredText("Open coach app", 140, C_GREY, 1);
  drawCenteredText("Profile > Pair Device", 156, C_GREY, 1);
  drawCenteredText("Enter the code above", 172, C_GREY, 1);
}

void drawWaitingForTag() {
  tft.fillScreen(GC9A01A_BLACK);
  drawCenteredText("READY", 30, GC9A01A_GREEN, 3);

  tft.drawCircle(120, 130, 32, GC9A01A_CYAN);
  tft.drawCircle(120, 130, 22, GC9A01A_CYAN);
  tft.drawCircle(120, 130, 12, GC9A01A_CYAN);

  drawCenteredText("Tap your tag", 180, GC9A01A_WHITE, 2);

  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(8, 220);
  tft.print("Ex: "); tft.print(activeExercise);
}

void drawSelectionScreen(const char* title, int total) {
  tft.fillScreen(GC9A01A_BLACK);
  tft.setTextColor(GC9A01A_CYAN);
  tft.setTextSize(2);
  tft.setCursor(6, 6);
  tft.println(title);
  tft.drawFastHLine(0, 28, 240, C_GREY);

  int y = 34;
  int rowH = 32;
  int visible = min(VISIBLE_ROWS, total - scrollOffset);

  for (int i = 0; i < visible; i++) {
    int idx = scrollOffset + i;
    bool hi = (i == cursorPos);
    if (hi) tft.fillRect(0, y, 240, rowH, GC9A01A_BLUE);
    tft.setTextColor(GC9A01A_WHITE);
    tft.setTextSize(2);
    tft.setCursor(6, y + 8);

    if (state == SELECT_PLAYER && idx < candidateCount) {
      Player &p = candidates[idx];
      if (p.jersey_number > 0) {
        tft.print("#");
        if (p.jersey_number < 10) tft.print("0");
        tft.print(p.jersey_number);
        tft.print(" ");
      }
      tft.print(p.first_name);
      tft.print(" ");
      tft.print(p.last_name[0]);
      tft.print(".");
    } else if (state == SELECT_EXERCISE && idx < NUM_EXERCISES) {
      tft.print(EXERCISES[idx]);
    }
    y += rowH;
  }

  if (total > VISIBLE_ROWS) {
    tft.setTextColor(C_GREY);
    tft.setTextSize(1);
    tft.setCursor(200, 8);
    tft.print(scrollOffset + cursorPos + 1);
    tft.print("/");
    tft.print(total);
  }

  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(6, 215);
  tft.print("GRN=Pick YEL=Down RED=Up");
}

void drawIdle() {
  tft.fillScreen(GC9A01A_BLACK);
  tft.setTextColor(GC9A01A_GREEN);
  tft.setTextSize(3);
  tft.setCursor(64, 8);
  tft.println("READY");

  tft.setTextSize(1);
  tft.setTextColor(GC9A01A_CYAN);
  tft.setCursor(6, 42);
  tft.print("Player: ");
  tft.setTextColor(GC9A01A_WHITE);
  if (currentPlayer.jersey_number > 0) {
    tft.print("#"); tft.print(currentPlayer.jersey_number); tft.print(" ");
  }
  tft.print(currentPlayer.first_name);
  tft.print(" ");
  tft.print(currentPlayer.last_name);

  tft.setTextColor(GC9A01A_CYAN);
  tft.setCursor(6, 56);
  tft.print("Exercise: ");
  tft.setTextColor(GC9A01A_WHITE);
  tft.print(activeExercise);

  if (lastSetReps > 0) {
    tft.setTextColor(GC9A01A_WHITE);
    tft.setTextSize(2);
    tft.setCursor(24, 80);
    tft.print("Last set: ");
    tft.print(lastSetReps);
    tft.print(" reps");
  }

  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(6, 165);
  tft.println("GRN=Start  YEL=Chg Exercise");
  tft.setCursor(6, 178);
  tft.println("YEL hold=Calibrate");
  tft.setCursor(6, 191);
  tft.println("RED=Eject Player");
  tft.setCursor(6, 204);
  tft.println("RED hold=Sleep");

  if (calibrated) {
    tft.setTextColor(GC9A01A_GREEN);
    tft.setCursor(170, 204);
    tft.println("CAL OK");
  }
}

void drawRunning() {
  tft.fillScreen(GC9A01A_BLACK);
  tft.setTextColor(GC9A01A_GREEN);
  tft.setTextSize(4);
  tft.setCursor(24, 4);
  tft.print("Rep ");
  tft.println(repCount);
  tft.drawFastHLine(10, 40, 220, C_GREY);

  if (lastRep.repNum == 0) {
    tft.setTextColor(GC9A01A_YELLOW);
    tft.setTextSize(2);
    tft.setCursor(48, 110);
    tft.println("Waiting...");
    return;
  }

  int y = 50, dy = 22;
  tft.setTextSize(2);
  tft.setTextColor(GC9A01A_CYAN); tft.setCursor(6, y);
  tft.print("Peak V "); tft.setTextColor(GC9A01A_WHITE);
  tft.print(lastRep.peakVel, 3); tft.println(" m/s"); y += dy;

  tft.setTextColor(GC9A01A_CYAN); tft.setCursor(6, y);
  tft.print("Mean V "); tft.setTextColor(GC9A01A_WHITE);
  tft.print(lastRep.meanVel, 3); tft.println(" m/s"); y += dy;

  tft.setTextColor(GC9A01A_CYAN); tft.setCursor(6, y);
  tft.print("ROM    "); tft.setTextColor(GC9A01A_WHITE);
  tft.print(lastRep.romCm, 1); tft.println(" cm"); y += dy;

  tft.setTextColor(GC9A01A_CYAN); tft.setCursor(6, y);
  tft.print("Conc   "); tft.setTextColor(GC9A01A_WHITE);
  tft.print(lastRep.concMs); tft.println(" ms"); y += dy;

  tft.setTextColor(GC9A01A_CYAN); tft.setCursor(6, y);
  tft.print("Ecc    "); tft.setTextColor(GC9A01A_WHITE);
  tft.print(lastRep.eccMs); tft.println(" ms");
}

void drawCalibrating() {
  tft.fillScreen(GC9A01A_BLACK);
  tft.setTextColor(GC9A01A_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(18, 90);
  tft.println("Calibrating...");
  tft.setTextColor(GC9A01A_WHITE);
  tft.setCursor(30, 130);
  tft.println("Hold still!");
}

void updateDisplay() {
  if (!displayDirty) return;
  displayDirty = false;
  switch (state) {
    case SPLASH:          drawSplashScreen(); break;
    case PAIRING:         drawPairingScreen(); break;
    case WAITING_FOR_TAG: drawWaitingForTag(); break;
    case SELECT_PLAYER:   drawSelectionScreen("Pick Player", candidateCount); break;
    case SELECT_EXERCISE: drawSelectionScreen("Pick Exercise", NUM_EXERCISES); break;
    case IDLE:            drawIdle(); break;
    case RUNNING:         drawRunning(); break;
    case CALIBRATING:     drawCalibrating(); break;
    default: break;
  }
}

// Show transient overlay messages without clobbering state.
void flashMessage(const char* line1, const char* line2, uint16_t color, int ms) {
  tft.fillScreen(GC9A01A_BLACK);
  drawCenteredText(line1, 90, color, 3);
  if (line2) drawCenteredText(line2, 140, GC9A01A_WHITE, 2);
  delay(ms);
  displayDirty = true;
}

// ══════════════════════════════════════════════════════════════════════
//  CALIBRATION
// ══════════════════════════════════════════════════════════════════════
static const int CAL_SAMPLES = 200;
int calCount = 0;
float calSumAx, calSumAy, calSumAz, calSumGx, calSumGy, calSumGz;

void startCalibration() {
  state = CALIBRATING;
  calCount = 0;
  calSumAx = calSumAy = calSumAz = 0;
  calSumGx = calSumGy = calSumGz = 0;
  Serial.println("CALIBRATING — hold still");
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
    biasGx = calSumGx / CAL_SAMPLES;
    biasGy = calSumGy / CAL_SAMPLES;
    biasGz = calSumGz / CAL_SAMPLES;
    calibrated = true;
    Serial.println("CAL_DONE");
    state = IDLE;
    displayDirty = true;
  }
}

// ══════════════════════════════════════════════════════════════════════
//  SET START / STOP / EJECT
// ══════════════════════════════════════════════════════════════════════
void startSet() {
  if (state != IDLE) return;
  if (!playerLoaded) {
    Serial.println("ERROR: no player");
    return;
  }
  if (!calibrated) {
    Serial.println("WARN: not calibrated");
  }
  state = RUNNING;
  resetPipeline();
  storedRepCount = 0;
  samplePoolUsed = 0;
  Serial.print("STARTED_SET  player=");
  Serial.print(currentPlayer.first_name); Serial.print(" "); Serial.print(currentPlayer.last_name);
  Serial.print("  exercise="); Serial.println(activeExercise);
  displayDirty = true;
}

void stopSet() {
  if (state != RUNNING) return;
  state = IDLE;
  lastSetReps = repCount;
  Serial.print("STOPPED_SET total_reps="); Serial.println(repCount);
  postSetData();
  // After upload, eject player so the next lifter taps in
  ejectPlayer();
}

void ejectPlayer() {
  playerLoaded = false;
  memset(&currentPlayer, 0, sizeof(currentPlayer));
  state = WAITING_FOR_TAG;
  displayDirty = true;
}

// ══════════════════════════════════════════════════════════════════════
//  REP SUMMARY (lifted from v6)
// ══════════════════════════════════════════════════════════════════════
void emitRepSummary() {
  float concMeanVel = (concSamples > 0) ? (concSumVel / (float)concSamples) : 0;
  float concDurMs = concEndMs - concStartMs;
  float eccDurMs  = millis() - eccStartMs;
  float totalDurMs = concDurMs + eccDurMs;
  float romCm = fabs(concDisplacement) * 100.0f;
  if (romCm < MIN_ROM_CM) return;

  repCount++;

  if (storedRepCount < 30) {
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

  Serial.print("REP_SUMMARY,"); Serial.print(repCount); Serial.print(",");
  Serial.print(concPeakVel, 3); Serial.print(",");
  Serial.print(concMeanVel, 3); Serial.print(",");
  Serial.print(romCm, 1); Serial.print(",");
  Serial.println((int)totalDurMs);

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
//  VBT PROCESSING (lifted from v6)
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
  float ugx = gex / gmag, ugy = gey / gmag, ugz = gez / gmag;
  float aAlongG = ax*ugx + ay*ugy + az*ugz;
  float aVert = aAlongG - G_CONST;

  aVert = smaFilter(aVert);
  if (fabs(aVert) < ACC_DEADBAND) aVert = 0;

  float gyroMag = sqrtf(gx*gx + gy*gy + gz*gz);
  if (fabs(aVert) < ZUPT_ACCEL_THRESH && gyroMag < ZUPT_GYRO_THRESH) zuptCounter++;
  else zuptCounter = 0;
  bool isStationary = (zuptCounter >= ZUPT_COUNT_THRESH);

  if (isStationary) vVert *= 0.90f;
  else { vVert += aVert * dt; vVert *= VEL_DECAY; }
  if (vVert >  VEL_CLAMP) vVert =  VEL_CLAMP;
  if (vVert < -VEL_CLAMP) vVert = -VEL_CLAMP;
  if (phase != REST) dVert += vVert * dt;

  unsigned long tms = millis();

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
        repSampleStartMs = tms;
        sampleDecimator = 0;
        repSamples[storedRepCount].startIdx = samplePoolUsed;
      }
      break;
    }
    case CONCENTRIC: {
      if (vVert > concPeakVel) concPeakVel = vVert;
      if (fabs(aVert) > concPeakAccel) concPeakAccel = fabs(aVert);
      concSumVel += vVert; concSamples++;
      concDisplacement = dVert;
      if (vVert < CONC_END_THRESH && (tms - concStartMs) > (unsigned long)MIN_CONC_MS) {
        concEndMs = tms;
        phase = ECCENTRIC;
        eccStartMs = tms;
        eccPeakVel = 0;
      }
      if ((tms - concStartMs) > 5000) { phase = REST; lastRepEndMs = tms; }
      break;
    }
    case ECCENTRIC: {
      if (vVert < eccPeakVel) eccPeakVel = vVert;
      if (fabs(aVert) > eccPeakAccel) eccPeakAccel = fabs(aVert);
      eccSumVel += fabs(vVert); eccSamples++;
      eccDisplacement = dVert - concDisplacement;
      bool hasMovedDown = (eccPeakVel < ECC_MOVE_THRESH);
      if (hasMovedDown && fabs(vVert) < ECC_END_THRESH && (tms - eccStartMs) > (unsigned long)MIN_ECC_MS) {
        emitRepSummary(); lastRepEndMs = tms; phase = REST;
      }
      if ((tms - eccStartMs) > 5000) {
        emitRepSummary(); lastRepEndMs = tms; phase = REST;
      }
      break;
    }
  }
}

// ══════════════════════════════════════════════════════════════════════
//  DEEP SLEEP
// ══════════════════════════════════════════════════════════════════════
void enterDeepSleep() {
  Serial.println("DEEP_SLEEP");
  tft.fillScreen(GC9A01A_BLACK);
  drawCenteredText("Sleeping...", 100, GC9A01A_MAGENTA, 2);
  drawCenteredText("Press button to wake", 130, C_GREY, 1);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  // Wake on the ladder pin going high (any button press pulls it up).
  // The ladder uses GPIO7 as ADC input with a 10K pull-down — pressing any
  // button raises voltage above 1.4V. ext0 wake on RTC IO low/high needs a
  // digital threshold; safest is a periodic timer wake every 2s and re-check.
  esp_sleep_enable_timer_wakeup(2ULL * 1000000ULL);
  esp_deep_sleep_start();
}

// ══════════════════════════════════════════════════════════════════════
//  NFC POLLING
// ══════════════════════════════════════════════════════════════════════
String formatUid(const uint8_t* uid, uint8_t len) {
  String s;
  s.reserve(len * 3);
  for (uint8_t i = 0; i < len; i++) {
    if (i > 0) s += ":";
    char buf[4];
    snprintf(buf, sizeof(buf), "%02X", uid[i]);
    s += buf;
  }
  return s;
}

unsigned long lastTagPollMs = 0;
String lastSeenUid = "";
unsigned long lastSeenAt = 0;

bool pollTag(String& uidOut) {
  if (!nfcOk) return false;
  if (millis() - lastTagPollMs < 200) return false;
  lastTagPollMs = millis();

  uint8_t uid[7];
  uint8_t uidLen = 0;
  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 100)) return false;

  String s = formatUid(uid, uidLen);
  // Debounce: ignore the same UID for 2s
  if (s == lastSeenUid && (millis() - lastSeenAt) < 2000) return false;
  lastSeenUid = s;
  lastSeenAt = millis();
  uidOut = s;
  return true;
}

// ══════════════════════════════════════════════════════════════════════
//  SETUP & LOOP
// ══════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  // Make Serial.print() non-blocking when USB is unplugged.
  // With USB CDC on boot, prints to disconnected USB block ~500ms each by
  // default — that's enough to starve the WiFi stack of CPU time during
  // connect, causing timeouts on battery while it works fine on USB.
  Serial.setTxTimeoutMs(0);
  delay(800);
  Serial.println();
  Serial.println("╔══════════════════════════════════════╗");
  Serial.println("║   VBT v7 — PCB v2.1 production       ║");
  Serial.println("╚══════════════════════════════════════╝");

  // Release GPIO43/44 in case ROM bootloader claimed them
  gpio_reset_pin((gpio_num_t)PN532_MISO);
  gpio_reset_pin((gpio_num_t)PN532_SS);

  Serial.print("DEVICE_ID = "); Serial.println(DEVICE_ID);

  // ── Display ──
  setupDisplay();
  drawCenteredText("VBT v7", 80, GC9A01A_WHITE, 3);
  drawCenteredText(DEVICE_ID, 130, C_GREY, 2);

  // ── IMU ──
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  delay(20);
  uint8_t who = 0;
  if (imuReadReg(REG_WHO_AM_I, who) && who == 0x05) {
    imuOk = true;
    Serial.print("IMU OK (WHO_AM_I=0x"); Serial.print(who, HEX); Serial.println(")");
  } else {
    Serial.print("IMU FAIL (WHO_AM_I=0x"); Serial.print(who, HEX); Serial.println(")");
  }
  setupQMI8658();

  // ── NFC ──
  nfc.begin();
  uint32_t ver = nfc.getFirmwareVersion();
  if (ver) {
    nfcOk = true;
    nfc.SAMConfig();
    Serial.print("NFC OK (v"); Serial.print((ver >> 16) & 0xFF);
    Serial.print("."); Serial.print((ver >> 8) & 0xFF); Serial.println(")");
  } else {
    Serial.println("NFC FAIL");
  }

  // ── Buttons ──
  analogReadResolution(12);
  pinMode(BTN_PIN, INPUT);

  tlsClient.setInsecure();

  // Splash screen on every boot — user presses Green to enter.
  state = SPLASH;
  displayDirty = true;
  lastMicros = micros();
}

unsigned long lastPairCheckMs = 0;

void loop() {
  handleButtons();

  // ── SPLASH: hold Aiken logo until user presses Start ──
  if (state == SPLASH) {
    updateDisplay();
    delay(50);
    return;
  }

  // ── WIFI_CONNECT: triggered by Start press from splash ──
  if (state == WIFI_CONNECT) {
    tft.fillRect(0, 200, 240, 40, GC9A01A_BLACK);
    drawCenteredText("Connecting WiFi...", 218, GC9A01A_YELLOW, 1);
    bool ok = connectWiFi();
    if (!ok) {
      flashMessage("WiFi", "FAILED", GC9A01A_RED, 1800);
      state = SPLASH;          // back to splash so user can retry
      displayDirty = true;
      return;
    }
    state = PAIRING;
    displayDirty = true;
    lastPairCheckMs = 0;       // trigger immediate /device/status check
    return;
  }

  // ── PAIRING: poll status until paired ──
  if (state == PAIRING) {
    updateDisplay();
    if (millis() - lastPairCheckMs > 3000) {
      lastPairCheckMs = millis();
      if (checkPairingStatus()) {
        Serial.print("Paired with: "); Serial.println(coachName);
        tft.fillScreen(GC9A01A_BLACK);
        drawCenteredText("Paired!", 80, GC9A01A_GREEN, 3);
        if (coachName.length() > 0) {
          drawCenteredText(coachName.c_str(), 130, GC9A01A_WHITE, 2);
        }
        delay(1500);
        state = WAITING_FOR_TAG;
        displayDirty = true;
      }
    }
    delay(50);
    return;
  }

  // ── WAITING_FOR_TAG: poll PN532 ──
  if (state == WAITING_FOR_TAG) {
    updateDisplay();
    String uid;
    if (pollTag(uid)) {
      Serial.print("Tag: "); Serial.println(uid);
      tft.fillScreen(GC9A01A_BLACK);
      drawCenteredText("Looking up", 100, GC9A01A_YELLOW, 2);
      int n = lookupTag(uid);
      if (n == 0) {
        flashMessage("Unknown", "tag", GC9A01A_RED, 1500);
      } else if (n == 1) {
        currentPlayer = candidates[0];
        playerLoaded = true;
        Serial.print("Player: ");
        Serial.print(currentPlayer.first_name); Serial.print(" "); Serial.println(currentPlayer.last_name);
        state = IDLE;
        displayDirty = true;
      } else {
        Serial.print("Multiple matches: "); Serial.println(n);
        resetScroll();
        state = SELECT_PLAYER;
        displayDirty = true;
      }
    }
    delay(50);
    return;
  }

  // ── SELECTION SCREENS / IDLE: pure UI, no IMU ──
  if (state == SELECT_PLAYER || state == SELECT_EXERCISE || state == IDLE) {
    updateDisplay();
    delay(20);
    return;
  }

  // ── CALIBRATING / RUNNING: read IMU ──
  float ax, ay, az, gx, gy, gz;
  if (!imuOk || !readIMU(ax, ay, az, gx, gy, gz)) { delay(5); return; }

  if (state == CALIBRATING) {
    updateDisplay();
    processCalibrationSample(ax, ay, az, gx, gy, gz);
    delay(5);
    return;
  }

  if (state == RUNNING) {
    updateDisplay();
    processVBT(ax, ay, az, gx, gy, gz);
    delay(2);  // ~200Hz loop
    return;
  }

  delay(20);
}
