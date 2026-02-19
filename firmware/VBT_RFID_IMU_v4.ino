/*
  VBT_RFID_IMU_v4.ino
  ESP32-S3 + QMI8658 IMU + TFT_eSPI Display (VBT core only)

  Based on v3 — simplified to VBT + display only:
    - Removed WiFi, Supabase, RFID, rep upload
    - Migrated display from Adafruit_ST7789 to TFT_eSPI
    - Display pins configured in TFT_eSPI User_Setup.h
      (MOSI=41, SCLK=40, CS=39, DC=38, RST=42)

  Hardware:
    IMU:     I2C  SDA=GPIO47, SCL=GPIO48, Addr=0x6B
    Display: SPI  via TFT_eSPI (pins in User_Setup.h)

  Libraries needed (Arduino Library Manager):
    1. "TFT_eSPI" by Bodmer

  Serial Commands:
    s           = Start set
    x           = Stop set
    c           = Calibrate IMU (hold still ~1 second)
    e:<name>    = Change exercise (e.g. e:Bench Press)
*/

#include <Wire.h>
#include <math.h>
#include <SPI.h>
#include <TFT_eSPI.h>

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
//  VBT STATE
// ══════════════════════════════════════════════════════════════════════
enum DeviceState { IDLE, RUNNING, CALIBRATING };
DeviceState state = IDLE;

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
//  DISPLAY
// ══════════════════════════════════════════════════════════════════════
void setupDisplay() {
  tft.begin();
  tft.setRotation(2);
  tft.fillScreen(TFT_BLACK);
}

void updateDisplay() {
  if (!displayDirty) return;
  displayDirty = false;

  tft.fillScreen(TFT_BLACK);

  if (state == IDLE) {
    tft.setTextColor(TFT_GREEN);
    tft.setTextSize(3);
    tft.setCursor(72, 10);
    tft.println("READY");

    // Show exercise
    tft.setTextColor(TFT_CYAN);
    tft.setTextSize(1);
    tft.setCursor(6, 42);
    tft.print("Exercise: ");
    tft.setTextColor(TFT_WHITE);
    tft.print(activeExercise);

    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(2);
    if (lastSetReps > 0) {
      tft.setCursor(24, 65);
      tft.print("Last set: ");
      tft.print(lastSetReps);
      tft.println(" reps");
    }

    tft.setTextColor(C_GREY);
    tft.setTextSize(1);
    tft.setCursor(30, 190);
    tft.println("'c' = Calibrate");
    tft.setCursor(30, 205);
    tft.println("'s' = Start Set");
    if (calibrated) {
      tft.setTextColor(TFT_GREEN);
      tft.setCursor(30, 225);
      tft.println("Calibrated");
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
    state = IDLE;

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
    Serial.println("READY - Press 's' to START set.");
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
  if (state == RUNNING) return;
  if (!calibrated) {
    Serial.println("WARNING: Not calibrated. Press 'c' first for best results.");
  }
  state = RUNNING;
  resetPipeline();
  Serial.println("STARTED_SET");
  Serial.print("  Exercise: "); Serial.println(activeExercise);
  Serial.println("t_ms,aVert,vVert,dVert,ax,ay,az,gx,gy,gz,rep,phase");
  displayDirty = true;
}

void stopSet() {
  if (state == IDLE) return;
  state = IDLE;
  lastSetReps = repCount;
  Serial.print("STOPPED_SET,total_reps=");
  Serial.println(repCount);
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

  switch (phase) {
    case REST: {
      if ((tms - lastRepEndMs) > (unsigned long)MIN_REST_MS && vVert > CONC_START_THRESH) {
        phase = CONCENTRIC;
        concStartMs = tms;
        resetRepMetrics();
        concPeakVel = vVert;
        dVert = 0;
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
  }

  Serial.print("Unknown command: ");
  Serial.println(cmd);
  Serial.println("Commands: s, x, c, e:<name>");
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
//  ARDUINO SETUP & LOOP
// ══════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(800);

  Serial.println("╔═══════════════════════════════════╗");
  Serial.println("║   VBT IMU v4 (TFT_eSPI)           ║");
  Serial.println("║   VBT + Display core only          ║");
  Serial.println("╚═══════════════════════════════════╝");

  // Display
  setupDisplay();

  // IMU (I2C)
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

  Serial.println();
  Serial.println("Commands:");
  Serial.println("  s           Start set");
  Serial.println("  x           Stop set");
  Serial.println("  c           Calibrate IMU");
  Serial.println("  e:<name>    Change exercise");
  Serial.println();

  lastMicros = micros();
  displayDirty = true;
}

void loop() {
  handleSerialInput();
  updateDisplay();

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
