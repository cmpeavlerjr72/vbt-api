/*
  VBT_RFID_IMU_v5.ino
  ESP32-S3 + QMI8658 IMU + TFT_eSPI Display + RC522 RFID + 3 Buttons

  Based on v4 — adds:
    - RC522 RFID via software SPI (bit-bang) to gate set recording
    - Physical buttons: Green=Start, Red=Stop/Sleep, Yellow=Calibrate
    - Deep sleep via long-press Red (3s)

  Hardware:
    IMU:     I2C  SDA=GPIO47, SCL=GPIO48, Addr=0x6B
    Display: SPI  via TFT_eSPI (pins in User_Setup.h)
             MOSI=41, SCLK=40, CS=39, DC=38, RST=42
    RC522:   SS=4, SCK=5, MOSI=3, MISO=7, RST=14 (hardware SPI on HSPI bus)
    Green:   GPIO9  (Start)
    Red:     GPIO13 (Stop / Deep Sleep)
    Yellow:  GPIO12 (Calibrate)

  Libraries needed (Arduino Library Manager):
    1. "TFT_eSPI" by Bodmer

  Serial Commands:
    s           = Start set
    x           = Stop set
    c           = Calibrate IMU (hold still ~1 second)
    e:<name>    = Change exercise (e.g. e:Bench Press)
    u:<uid>     = Set player UID manually (e.g. u:AA:BB:CC:DD)

  Buttons:
    Green       = Start set
    Yellow      = Calibrate
    Red (short) = Stop set
    Red (3s)    = Deep sleep (any button wakes)
*/

#include <Wire.h>
#include <math.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

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
//  RC522 HARDWARE SPI (dedicated HSPI bus, independent from display)
// ══════════════════════════════════════════════════════════════════════
#define RFID_SS    4
#define RFID_SCK   5
#define RFID_MOSI  3
#define RFID_MISO  7
#define RFID_RST  14

// Use the second hardware SPI bus so display SPI can't interfere
#ifndef HSPI
#define HSPI 2  // SPI3_HOST on ESP32-S3
#endif
SPIClass rfidSPI(HSPI);
static const SPISettings rfidSPISettings(4000000, MSBFIRST, SPI_MODE0);

// RC522 registers
#define RC_REG_Command     0x01
#define RC_REG_ComIEn      0x02
#define RC_REG_ComIrq      0x04
#define RC_REG_Error       0x06
#define RC_REG_FIFOData    0x09
#define RC_REG_FIFOLevel   0x0A
#define RC_REG_Control     0x0C
#define RC_REG_BitFraming  0x0D
#define RC_REG_Coll        0x0E
#define RC_REG_Mode        0x11
#define RC_REG_TxMode      0x12
#define RC_REG_RxMode      0x13
#define RC_REG_TxControl   0x14
#define RC_REG_TxASK       0x15
#define RC_REG_ModWidth    0x24
#define RC_REG_TModeReg    0x2A
#define RC_REG_TPrescaler  0x2B
#define RC_REG_TReloadH    0x2C
#define RC_REG_TReloadL    0x2D
#define RC_REG_Version     0x37

// RC522 commands
#define CMD_Idle        0x00
#define CMD_CalcCRC     0x03
#define CMD_Transceive  0x0C
#define CMD_SoftReset   0x0F

// PICC commands
#define PICC_REQA       0x26
#define PICC_WUPA       0x52
#define PICC_ANTICOLL   0x93
#define PICC_HLTA       0x50

// RFID state
bool rfidOk = false;
bool rfidScanned = false;
String scannedUid = "";
unsigned long lastRfidPoll = 0;
String lastDetectedUid = "";
unsigned long lastDetectedTime = 0;

static const unsigned long RFID_POLL_MS    = 300;
static const unsigned long RFID_DEBOUNCE_MS = 1500;

byte rc522ReadReg(byte reg) {
  byte addr = ((reg << 1) & 0x7E) | 0x80;
  rfidSPI.beginTransaction(rfidSPISettings);
  digitalWrite(RFID_SS, LOW);
  rfidSPI.transfer(addr);
  byte val = rfidSPI.transfer(0x00);
  digitalWrite(RFID_SS, HIGH);
  rfidSPI.endTransaction();
  return val;
}

void rc522WriteReg(byte reg, byte val) {
  byte addr = (reg << 1) & 0x7E;
  rfidSPI.beginTransaction(rfidSPISettings);
  digitalWrite(RFID_SS, LOW);
  rfidSPI.transfer(addr);
  rfidSPI.transfer(val);
  digitalWrite(RFID_SS, HIGH);
  rfidSPI.endTransaction();
}

void rc522SetBitMask(byte reg, byte mask) {
  rc522WriteReg(reg, rc522ReadReg(reg) | mask);
}

void rc522ClearBitMask(byte reg, byte mask) {
  rc522WriteReg(reg, rc522ReadReg(reg) & ~mask);
}

void rc522Init() {
  rc522WriteReg(RC_REG_TxMode, 0x00);
  rc522WriteReg(RC_REG_RxMode, 0x00);
  rc522WriteReg(RC_REG_ModWidth, 0x26);

  // timer: TAuto=1, prescaler=169 → f_timer=40kHz, reload=1000 → 25ms timeout
  rc522WriteReg(RC_REG_TModeReg, 0x80);
  rc522WriteReg(RC_REG_TPrescaler, 0xA9);
  rc522WriteReg(RC_REG_TReloadH, 0x03);
  rc522WriteReg(RC_REG_TReloadL, 0xE8);

  // force 100% ASK modulation
  rc522WriteReg(RC_REG_TxASK, 0x40);

  // CRC preset 6363h (ISO 14443-3)
  rc522WriteReg(RC_REG_Mode, 0x3D);

  // turn on antenna
  rc522SetBitMask(RC_REG_TxControl, 0x03);
}

byte rc522Transceive(byte *sendData, byte sendLen, byte *recvData, byte *recvLen, byte *validBits) {
  byte txLastBits = validBits ? *validBits : 0;
  byte bitFraming = txLastBits;

  rc522WriteReg(RC_REG_Command, CMD_Idle);
  rc522WriteReg(RC_REG_ComIrq, 0x7F);
  rc522WriteReg(RC_REG_FIFOLevel, 0x80);   // flush FIFO

  for (byte i = 0; i < sendLen; i++) {
    rc522WriteReg(RC_REG_FIFOData, sendData[i]);
  }

  rc522WriteReg(RC_REG_BitFraming, bitFraming);
  rc522WriteReg(RC_REG_Command, CMD_Transceive);
  rc522SetBitMask(RC_REG_BitFraming, 0x80);  // StartSend

  const unsigned long deadline = millis() + 40;
  bool completed = false;
  byte irq = 0;
  do {
    irq = rc522ReadReg(RC_REG_ComIrq);
    if (irq & 0x30) { completed = true; break; }  // RxIRq or IdleIRq
    if (irq & 0x01) return 1;                      // TimerIRq = timeout
  } while (millis() < deadline);

  rc522ClearBitMask(RC_REG_BitFraming, 0x80);

  if (!completed) return 1;

  byte errorReg = rc522ReadReg(RC_REG_Error);
  if (errorReg & 0x13) return 2;  // BufferOvfl, ParityErr, ProtocolErr

  byte n = rc522ReadReg(RC_REG_FIFOLevel);
  if (recvLen) {
    byte maxRead = *recvLen;
    *recvLen = n;
    if (n > maxRead) n = maxRead;
  }
  for (byte i = 0; i < n; i++) {
    recvData[i] = rc522ReadReg(RC_REG_FIFOData);
  }

  return 0;
}

bool rc522RequestCard(byte *atqa, bool wakeup = false) {
  rc522ClearBitMask(RC_REG_Coll, 0x80);
  byte cmd = wakeup ? PICC_WUPA : PICC_REQA;
  byte recvLen = 2;
  byte validBits = 0x07;  // short frame: 7 bits
  byte result = rc522Transceive(&cmd, 1, atqa, &recvLen, &validBits);
  return (result == 0 && recvLen == 2);
}

bool rc522AntiCollision(byte *uid) {
  rc522WriteReg(RC_REG_BitFraming, 0x00);
  byte cmd[2] = { PICC_ANTICOLL, 0x20 };
  byte recvLen = 5;
  byte result = rc522Transceive(cmd, 2, uid, &recvLen, NULL);
  if (result != 0 || recvLen != 5) return false;
  byte bcc = uid[0] ^ uid[1] ^ uid[2] ^ uid[3];
  return (bcc == uid[4]);
}

void rc522HaltCard() {
  byte cmd[2] = { PICC_HLTA, 0x00 };
  byte recv[2];
  byte recvLen = 2;
  rc522Transceive(cmd, 2, recv, &recvLen, NULL);
}

String uidToString(byte *uid, byte len) {
  String s = "";
  for (byte i = 0; i < len; i++) {
    if (i > 0) s += ":";
    if (uid[i] < 0x10) s += "0";
    s += String(uid[i], HEX);
  }
  s.toUpperCase();
  return s;
}

// Init RC522 on dedicated HSPI bus — separate from TFT_eSPI's SPI bus
void setupRFID() {
  // Release boot ROM claims on GPIO 3 (JTAG) and GPIO 14 (SPIWP)
  gpio_reset_pin((gpio_num_t)RFID_MOSI);
  gpio_reset_pin((gpio_num_t)RFID_RST);

  // Initialize RFID on its own hardware SPI bus (HSPI/SPI3)
  // Display uses FSPI/SPI2 — completely independent, no pin conflicts
  rfidSPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, -1);  // SS managed manually
  Serial.println("RFID: HSPI bus initialized (SCK=5, MISO=7, MOSI=3)");

  // SS and RST managed as plain GPIO
  pinMode(RFID_SS, OUTPUT);
  digitalWrite(RFID_SS, HIGH);
  pinMode(RFID_RST, OUTPUT);
  digitalWrite(RFID_RST, HIGH);
  delay(10);

  // Hard reset
  Serial.println("RFID: Hard reset...");
  digitalWrite(RFID_RST, LOW);
  delay(50);
  digitalWrite(RFID_RST, HIGH);
  delay(50);

  byte version = rc522ReadReg(RC_REG_Version);
  Serial.print("RFID: VersionReg=0x");
  Serial.print(version, HEX);

  if (version == 0x91 || version == 0x92) {
    rfidOk = true;
    Serial.println(" OK");
    rc522Init();
  } else if (version == 0x00 || version == 0xFF) {
    rfidOk = false;
    Serial.println(" FAIL (check wiring)");
  } else {
    rfidOk = true;
    rc522Init();
    Serial.println(" OK (non-standard)");
  }
}

static unsigned long rfidPollCount = 0;

void pollRFID() {
  if (!rfidOk) return;
  unsigned long now = millis();
  if (now - lastRfidPoll < RFID_POLL_MS) return;
  lastRfidPoll = now;
  rfidPollCount++;

  // No pin reclaim needed — RFID has its own hardware SPI bus

  // Debug: verify SPI works (every 10th poll)
  if (rfidPollCount % 10 == 1) {
    byte ver = rc522ReadReg(RC_REG_Version);
    byte txCtrl = rc522ReadReg(RC_REG_TxControl);
    Serial.print("RFID_DBG: poll#");
    Serial.print(rfidPollCount);
    Serial.print(" ver=0x");
    Serial.print(ver, HEX);
    Serial.print(" txCtrl=0x");
    Serial.println(txCtrl, HEX);
  }

  byte atqa[2];
  if (!rc522RequestCard(atqa, true)) return;  // WUPA

  byte uid[5];
  if (!rc522AntiCollision(uid)) return;

  String uidStr = uidToString(uid, 4);

  // Debounce: ignore same card within 1.5s
  if (uidStr == lastDetectedUid && (now - lastDetectedTime) < RFID_DEBOUNCE_MS) {
    rc522HaltCard();
    return;
  }

  lastDetectedUid = uidStr;
  lastDetectedTime = now;
  scannedUid = uidStr;
  rfidScanned = true;

  Serial.print("RFID_SCAN,uid=");
  Serial.println(scannedUid);

  displayDirty = true;
  rc522HaltCard();
}

// ══════════════════════════════════════════════════════════════════════
//  DEVICE STATE (declared early so buttons can reference it)
// ══════════════════════════════════════════════════════════════════════
enum DeviceState { IDLE, RUNNING, CALIBRATING };
DeviceState state = IDLE;

// ══════════════════════════════════════════════════════════════════════
//  BUTTONS
// ══════════════════════════════════════════════════════════════════════
static const int BTN_GREEN  = 9;
static const int BTN_RED    = 13;
static const int BTN_YELLOW = 12;

static const unsigned long BTN_DEBOUNCE_MS    = 50;
static const unsigned long RED_LONG_PRESS_MS  = 3000;

bool btnGreenState = false, btnRedState = false, btnYellowState = false;
unsigned long btnGreenLastChange = 0, btnRedLastChange = 0, btnYellowLastChange = 0;
unsigned long redPressStart = 0;
bool redLongFired = false;

// Forward declarations
void startSet();
void stopSet();
void startCalibration();
void enterDeepSleep();

void setupButtons() {
  pinMode(BTN_GREEN,  INPUT_PULLUP);
  pinMode(BTN_RED,    INPUT_PULLUP);
  pinMode(BTN_YELLOW, INPUT_PULLUP);
  Serial.println("BTNS: Configured (9=Green, 13=Red, 12=Yellow) INPUT_PULLUP");
}

void handleButtons() {
  unsigned long now = millis();

  // Read raw (LOW = pressed with INPUT_PULLUP)
  bool greenNow  = (digitalRead(BTN_GREEN)  == LOW);
  bool redNow    = (digitalRead(BTN_RED)    == LOW);
  bool yellowNow = (digitalRead(BTN_YELLOW) == LOW);

  // Green — fire on press → startSet()
  if (greenNow != btnGreenState && (now - btnGreenLastChange) > BTN_DEBOUNCE_MS) {
    btnGreenState = greenNow;
    btnGreenLastChange = now;
    if (greenNow) {
      startSet();
    }
  }

  // Yellow — fire on press → startCalibration()
  if (yellowNow != btnYellowState && (now - btnYellowLastChange) > BTN_DEBOUNCE_MS) {
    btnYellowState = yellowNow;
    btnYellowLastChange = now;
    if (yellowNow && state != CALIBRATING) {
      startCalibration();
    }
  }

  // Red — short press = stop (on release), long hold 3s = deep sleep
  if (redNow != btnRedState && (now - btnRedLastChange) > BTN_DEBOUNCE_MS) {
    btnRedState = redNow;
    btnRedLastChange = now;
    if (redNow) {
      // Pressed
      redPressStart = now;
      redLongFired = false;
    } else {
      // Released — stop set if it wasn't a long press
      if (!redLongFired) {
        stopSet();
      }
    }
  }

  // Check for long press while held
  if (btnRedState && !redLongFired && (now - redPressStart) >= RED_LONG_PRESS_MS) {
    redLongFired = true;
    enterDeepSleep();
  }
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
//  DISPLAY RENDERING
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

    // Exercise
    tft.setTextColor(TFT_CYAN);
    tft.setTextSize(1);
    tft.setCursor(6, 42);
    tft.print("Exercise: ");
    tft.setTextColor(TFT_WHITE);
    tft.print(activeExercise);

    // RFID status
    tft.setTextSize(1);
    tft.setCursor(6, 56);
    if (rfidScanned) {
      tft.setTextColor(TFT_GREEN);
      tft.print("Player: ");
      tft.setTextColor(TFT_WHITE);
      tft.print(scannedUid);
    } else {
      tft.setTextColor(TFT_YELLOW);
      tft.print("Scan card...");
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
    tft.setCursor(12, 190);
    tft.println("GRN = Start Set");
    tft.setCursor(12, 205);
    tft.println("YEL = Calibrate");
    tft.setCursor(12, 220);
    tft.println("RED hold = Sleep");
    if (calibrated) {
      tft.setTextColor(TFT_GREEN);
      tft.setCursor(160, 220);
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
    Serial.println("READY - Press Green or 's' to START set.");
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
  if (state != IDLE) return;   // changed from == RUNNING: block during calibration too
  if (!rfidScanned) {
    Serial.println("ERROR: Scan RFID card before starting set.");
    // Brief error flash on display
    tft.fillRect(0, 100, 240, 30, TFT_BLACK);
    tft.setTextColor(TFT_RED);
    tft.setTextSize(2);
    tft.setCursor(6, 105);
    tft.print("Scan card first!");
    return;
  }
  if (!calibrated) {
    Serial.println("WARNING: Not calibrated. Press Yellow or 'c' first for best results.");
  }
  state = RUNNING;
  resetPipeline();
  Serial.println("STARTED_SET");
  Serial.print("  Player: "); Serial.println(scannedUid);
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

    if (prefix == "u" || prefix == "U") {
      scannedUid = value;
      rfidScanned = true;
      Serial.print("UID set manually: ");
      Serial.println(scannedUid);
      displayDirty = true;
      return;
    }
  }

  Serial.print("Unknown command: ");
  Serial.println(cmd);
  Serial.println("Commands: s, x, c, e:<name>, u:<uid>");
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

  // Turn off RFID antenna
  if (rfidOk) {
    rc522ClearBitMask(RC_REG_TxControl, 0x03);
  }

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

  Serial.println("╔═══════════════════════════════════╗");
  Serial.println("║   VBT IMU v5 (RFID + Buttons)     ║");
  Serial.println("║   Full VBT + RFID gate + Buttons   ║");
  Serial.println("╚═══════════════════════════════════╝");

  setupButtons();

  // RFID on HSPI bus (SPI3) — independent from display's FSPI bus (SPI2)
  setupRFID();

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

  // Display on FSPI bus (SPI2) — no conflict with RFID's HSPI bus
  setupDisplay();

  Serial.println();
  Serial.println("Commands:");
  Serial.println("  s           Start set (or Green button)");
  Serial.println("  x           Stop set (or Red button)");
  Serial.println("  c           Calibrate IMU (or Yellow button)");
  Serial.println("  e:<name>    Change exercise");
  Serial.println("  u:<uid>     Set player UID manually");
  Serial.println("  Red hold 3s = Deep sleep");
  Serial.println();

  lastMicros = micros();
  displayDirty = true;
}

void loop() {
  handleSerialInput();
  handleButtons();
  updateDisplay();

  if (state == IDLE) {
    pollRFID();
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
