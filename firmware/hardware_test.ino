/*
  hardware_test.ino
  Connection tester for Waveshare ESP32-S3 + RC522 + 3 Buttons

  Tests all peripherals and reports pass/fail on display + serial.
  RC522 uses SOFTWARE SPI (bit-bang) to avoid bus conflicts with TFT_eSPI.
  Includes full RFID card scanning (no MFRC522 library needed).

  Pin Map:
    Display: TFT_eSPI (MOSI=41, SCLK=40, CS=39, DC=38, RST=42) — on-board
    IMU:     I2C  SDA=47, SCL=48, Addr=0x6B — on-board
    RC522:   SS=4, SCK=5, MOSI=3, MISO=7, RST=14
    Green:   GPIO9  (Start)
    Red:     GPIO13 (Stop / Sleep)
    Yellow:  GPIO10 (Calibrate)

  Libraries needed:
    1. "TFT_eSPI" by Bodmer
*/

#include <Wire.h>
#include <TFT_eSPI.h>
#include <driver/gpio.h>

// ── Display ──
TFT_eSPI tft = TFT_eSPI(240, 240);

// ── IMU ──
#define I2C_SDA 47
#define I2C_SCL 48
#define QMI_ADDR 0x6B

// ── RC522 (software SPI) ──
#define RFID_SS    4
#define RFID_SCK   5
#define RFID_MOSI  3
#define RFID_MISO  7
#define RFID_RST  14

// RC522 registers
#define REG_Command     0x01
#define REG_ComIEn      0x02
#define REG_ComIrq      0x04
#define REG_Error       0x06
#define REG_FIFOData    0x09
#define REG_FIFOLevel   0x0A
#define REG_Control     0x0C
#define REG_BitFraming  0x0D
#define REG_Coll        0x0E
#define REG_Mode        0x11
#define REG_TxMode      0x12
#define REG_RxMode      0x13
#define REG_TxControl   0x14
#define REG_TxASK       0x15
#define REG_ModWidth    0x24
#define REG_TModeReg    0x2A
#define REG_TPrescaler  0x2B
#define REG_TReloadH    0x2C
#define REG_TReloadL    0x2D
#define REG_Version     0x37

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

// ── Buttons ──
#define BTN_GREEN  9
#define BTN_RED   13
#define BTN_YELLOW 12

// ── State ──
bool imuOk   = false;
bool rfidOk  = false;
bool displayOk = false;

bool greenPressed  = false;
bool redPressed    = false;
bool yellowPressed = false;

bool greenSeen  = false;
bool redSeen    = false;
bool yellowSeen = false;

unsigned long lastRefresh = 0;
unsigned long lastRfidCheck = 0;
String lastScannedUid = "";
unsigned long lastScanTime = 0;

// ══════════════════════════════════════════════════════════════════════
//  SOFTWARE SPI for RC522 (bit-bang, SPI Mode 0)
// ══════════════════════════════════════════════════════════════════════
byte softSpiTransfer(byte data) {
  byte reply = 0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(RFID_MOSI, (data >> i) & 1);
    delayMicroseconds(4);
    digitalWrite(RFID_SCK, HIGH);
    delayMicroseconds(4);
    if (digitalRead(RFID_MISO)) reply |= (1 << i);
    digitalWrite(RFID_SCK, LOW);
    delayMicroseconds(4);
  }
  return reply;
}

byte rc522ReadReg(byte reg) {
  byte addr = ((reg << 1) & 0x7E) | 0x80;
  digitalWrite(RFID_SS, LOW);
  delayMicroseconds(10);
  softSpiTransfer(addr);
  delayMicroseconds(10);
  byte val = softSpiTransfer(0x00);
  digitalWrite(RFID_SS, HIGH);
  delayMicroseconds(10);
  return val;
}

void rc522WriteReg(byte reg, byte val) {
  byte addr = (reg << 1) & 0x7E;
  digitalWrite(RFID_SS, LOW);
  delayMicroseconds(10);
  softSpiTransfer(addr);
  delayMicroseconds(10);
  softSpiTransfer(val);
  delayMicroseconds(10);
  digitalWrite(RFID_SS, HIGH);
  delayMicroseconds(10);
}

void rc522SetBitMask(byte reg, byte mask) {
  rc522WriteReg(reg, rc522ReadReg(reg) | mask);
}

void rc522ClearBitMask(byte reg, byte mask) {
  rc522WriteReg(reg, rc522ReadReg(reg) & ~mask);
}

// ══════════════════════════════════════════════════════════════════════
//  RC522 INIT & CARD PROTOCOL
// ══════════════════════════════════════════════════════════════════════
void rc522Init() {
  // NO soft reset here — hard reset was already done in setup().
  // The MFRC522 library skips soft reset after hard reset too.
  // Doing a soft reset puts the chip into PowerDown mode which
  // our bit-bang SPI can't reliably wake from.

  // reset baud rates and modulation width (match MFRC522 library exactly)
  rc522WriteReg(REG_TxMode, 0x00);
  rc522WriteReg(REG_RxMode, 0x00);
  rc522WriteReg(REG_ModWidth, 0x26);

  // timer: TAuto=1, prescaler=0x0A9=169 → f_timer=40kHz (25μs period)
  // reload=0x03E8=1000 → 25ms timeout
  rc522WriteReg(REG_TModeReg, 0x80);
  rc522WriteReg(REG_TPrescaler, 0xA9);
  rc522WriteReg(REG_TReloadH, 0x03);
  rc522WriteReg(REG_TReloadL, 0xE8);

  // force 100% ASK modulation
  rc522WriteReg(REG_TxASK, 0x40);

  // CRC preset 6363h (ISO 14443-3)
  rc522WriteReg(REG_Mode, 0x3D);

  // turn on antenna
  rc522SetBitMask(REG_TxControl, 0x03);
}

// Communicate with a PICC via Transceive command
// Returns 0 on success, non-zero on error
byte rc522Transceive(byte *sendData, byte sendLen, byte *recvData, byte *recvLen, byte *validBits, bool debug = false) {
  byte txLastBits = validBits ? *validBits : 0;
  byte bitFraming = txLastBits;  // rxAlign=0

  rc522WriteReg(REG_Command, CMD_Idle);       // stop any active command
  rc522WriteReg(REG_ComIrq, 0x7F);           // clear all IRQ flags
  rc522WriteReg(REG_FIFOLevel, 0x80);        // flush FIFO (direct write, NOT read-modify-write)

  // write data to FIFO
  for (byte i = 0; i < sendLen; i++) {
    rc522WriteReg(REG_FIFOData, sendData[i]);
  }

  rc522WriteReg(REG_BitFraming, bitFraming);  // set bit framing
  rc522WriteReg(REG_Command, CMD_Transceive); // execute command
  rc522SetBitMask(REG_BitFraming, 0x80);      // StartSend

  // wait for completion or timeout (~36ms)
  const unsigned long deadline = millis() + 40;
  bool completed = false;
  byte irq = 0;
  do {
    irq = rc522ReadReg(REG_ComIrq);
    if (irq & 0x30) { completed = true; break; }  // RxIRq or IdleIRq
    if (irq & 0x01) {                              // TimerIRq
      if (debug) Serial.printf("  [transceive] TIMEOUT irq=0x%02X\n", irq);
      return 1;
    }
  } while (millis() < deadline);

  rc522ClearBitMask(REG_BitFraming, 0x80);   // clear StartSend

  if (!completed) {
    if (debug) Serial.printf("  [transceive] DEADLINE irq=0x%02X\n", irq);
    return 1;
  }

  byte errorReg = rc522ReadReg(REG_Error);
  if (debug) Serial.printf("  [transceive] irq=0x%02X err=0x%02X\n", irq, errorReg);

  if (errorReg & 0x13) return 2;  // BufferOvfl, ParityErr, ProtocolErr

  // read received data
  byte n = rc522ReadReg(REG_FIFOLevel);
  if (debug) Serial.printf("  [transceive] FIFO=%d bytes\n", n);

  if (recvLen) {
    byte maxRead = *recvLen;
    *recvLen = n;
    if (n > maxRead) n = maxRead;
  }
  for (byte i = 0; i < n; i++) {
    recvData[i] = rc522ReadReg(REG_FIFOData);
    if (debug) Serial.printf("  [transceive] data[%d]=0x%02X\n", i, recvData[i]);
  }

  return 0;  // success
}

// Send REQA or WUPA — returns true if a card responded
bool rc522RequestCard(byte *atqa, bool wakeup = false) {
  rc522ClearBitMask(REG_Coll, 0x80);    // ValuesAfterColl: clear to keep bits after collision
  byte cmd = wakeup ? PICC_WUPA : PICC_REQA;
  byte recvLen = 2;
  byte validBits = 0x07;  // short frame: 7 bits
  byte result = rc522Transceive(&cmd, 1, atqa, &recvLen, &validBits);
  return (result == 0 && recvLen == 2);
}

// Anti-collision — returns true if UID read successfully
// uid must be at least 5 bytes (4 UID + 1 BCC)
bool rc522AntiCollision(byte *uid) {
  rc522WriteReg(REG_BitFraming, 0x00);
  byte cmd[2] = { PICC_ANTICOLL, 0x20 };  // SEL + NVB (2 bytes sent, 0 bits)
  byte recvLen = 5;
  byte result = rc522Transceive(cmd, 2, uid, &recvLen, NULL);
  if (result != 0 || recvLen != 5) return false;

  // verify BCC (XOR of 4 UID bytes should equal 5th byte)
  byte bcc = uid[0] ^ uid[1] ^ uid[2] ^ uid[3];
  return (bcc == uid[4]);
}

// Halt the card (stop it from responding)
void rc522HaltCard() {
  byte cmd[2] = { PICC_HLTA, 0x00 };
  byte recv[2];
  byte recvLen = 2;
  rc522Transceive(cmd, 2, recv, &recvLen, NULL);
}

// Convert UID bytes to uppercase hex string
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

// ══════════════════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=================================");
  Serial.println("  HARDWARE CONNECTION TESTER v4");
  Serial.println("=================================");
  Serial.println();

  // — Buttons —
  pinMode(BTN_GREEN,  INPUT_PULLUP);
  pinMode(BTN_RED,    INPUT_PULLUP);
  pinMode(BTN_YELLOW, INPUT_PULLUP);
  Serial.println("[BTNS]  Pins configured (9, 13, 10) INPUT_PULLUP");

  // — Display —
  Serial.print("[DISP]  Initializing TFT_eSPI... ");
  tft.init();
  tft.setRotation(2);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("HW TEST v1");
  displayOk = true;
  Serial.println("OK");

  // — IMU —
  Serial.print("[IMU]   Checking QMI8658 at 0x6B... ");
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.beginTransmission(QMI_ADDR);
  if (Wire.endTransmission() == 0) {
    Wire.beginTransmission(QMI_ADDR);
    Wire.write(0x00);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)QMI_ADDR, (uint8_t)1);
    if (Wire.available()) {
      uint8_t whoami = Wire.read();
      Serial.printf("WHO_AM_I = 0x%02X ", whoami);
      if (whoami == 0x05) {
        imuOk = true;
        Serial.println("OK");
      } else {
        Serial.println("UNEXPECTED (expected 0x05)");
      }
    }
  } else {
    Serial.println("NOT FOUND");
  }

  // — RC522 (software SPI) —
  // Forcibly reclaim RFID pins from SPI peripheral matrix
  // (TFT_eSPI's init hijacks GPIOs through the ESP32 SPI hardware)
  Serial.println("[RFID]  Reclaiming RFID pins from SPI peripheral...");
  gpio_reset_pin((gpio_num_t)RFID_SS);
  gpio_reset_pin((gpio_num_t)RFID_SCK);
  gpio_reset_pin((gpio_num_t)RFID_MOSI);
  gpio_reset_pin((gpio_num_t)RFID_MISO);
  gpio_reset_pin((gpio_num_t)RFID_RST);

  pinMode(RFID_SS,   OUTPUT);
  pinMode(RFID_SCK,  OUTPUT);
  pinMode(RFID_MOSI, OUTPUT);
  pinMode(RFID_MISO, INPUT);
  pinMode(RFID_RST,  OUTPUT);

  digitalWrite(RFID_SS,  HIGH);
  digitalWrite(RFID_SCK, LOW);
  digitalWrite(RFID_MOSI, LOW);

  // hard reset
  Serial.println("[RFID]  Hard reset...");
  digitalWrite(RFID_RST, LOW);
  delay(50);
  digitalWrite(RFID_RST, HIGH);
  delay(50);

  // verify writes work: write 0xAA to TxModeReg, read back, restore
  byte origTxMode = rc522ReadReg(REG_TxMode);
  rc522WriteReg(REG_TxMode, 0xAA);
  byte readback = rc522ReadReg(REG_TxMode);
  rc522WriteReg(REG_TxMode, origTxMode);
  Serial.printf("        Write test: wrote 0xAA to TxMode, read 0x%02X %s\n",
    readback, readback == 0xAA ? "OK" : "FAIL");

  // also read Command reg right after hard reset (before init)
  byte cmdBeforeInit = rc522ReadReg(REG_Command);
  Serial.printf("        Command before init = 0x%02X (expect 0x20)\n", cmdBeforeInit);

  byte version = rc522ReadReg(REG_Version);
  Serial.printf("        VersionReg = 0x%02X ", version);

  if (version == 0x91 || version == 0x92) {
    rfidOk = true;
    Serial.println("OK");
    rc522Init();

    // verify key registers after init
    Serial.println("        Register dump after init:");
    Serial.printf("          TxControl = 0x%02X (expect 0x03 bits set)\n", rc522ReadReg(REG_TxControl));
    Serial.printf("          TxASK     = 0x%02X (expect 0x40)\n", rc522ReadReg(REG_TxASK));
    Serial.printf("          Mode      = 0x%02X (expect 0x3D)\n", rc522ReadReg(REG_Mode));
    Serial.printf("          TMode     = 0x%02X (expect 0x80)\n", rc522ReadReg(REG_TModeReg));
    Serial.printf("          TPrescaler= 0x%02X (expect 0xA9)\n", rc522ReadReg(REG_TPrescaler));
    Serial.printf("          TReloadH  = 0x%02X (expect 0x03)\n", rc522ReadReg(REG_TReloadH));
    Serial.printf("          TReloadL  = 0x%02X (expect 0xE8)\n", rc522ReadReg(REG_TReloadL));
    Serial.printf("          TxMode    = 0x%02X (expect 0x00)\n", rc522ReadReg(REG_TxMode));
    Serial.printf("          RxMode    = 0x%02X (expect 0x00)\n", rc522ReadReg(REG_RxMode));
    Serial.printf("          Command   = 0x%02X (expect 0x00=Idle)\n", rc522ReadReg(REG_Command));
    Serial.printf("          ComIrq    = 0x%02X\n", rc522ReadReg(REG_ComIrq));
    Serial.printf("          Error     = 0x%02X\n", rc522ReadReg(REG_Error));
    Serial.printf("          FIFOLevel = 0x%02X\n", rc522ReadReg(REG_FIFOLevel));
    Serial.println("        Antenna ON — ready to scan cards");
  } else if (version == 0x00) {
    Serial.println("FAIL (no response — check wiring)");
  } else if (version == 0xFF) {
    Serial.println("FAIL (all ones — MISO stuck HIGH)");
  } else {
    rfidOk = true;
    rc522Init();
    Serial.println("OK (non-standard but responding)");
  }

  // — Summary —
  Serial.println();
  Serial.println("--- STARTUP RESULTS ---");
  Serial.printf("  Display : %s\n", displayOk ? "PASS" : "FAIL");
  Serial.printf("  IMU     : %s\n", imuOk     ? "PASS" : "FAIL");
  Serial.printf("  RC522   : %s\n", rfidOk    ? "PASS" : "FAIL");
  Serial.println("  Buttons : press each to test");
  if (rfidOk) {
    Serial.println("  RFID    : scanning for cards...");
  }
  Serial.println();

  drawScreen();
}

// ══════════════════════════════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════════════════════════════
void loop() {
  bool g = digitalRead(BTN_GREEN)  == LOW;
  bool r = digitalRead(BTN_RED)    == LOW;
  bool y = digitalRead(BTN_YELLOW) == LOW;

  bool changed = false;

  if (g != greenPressed)  { greenPressed  = g; changed = true; }
  if (r != redPressed)    { redPressed    = r; changed = true; }
  if (y != yellowPressed) { yellowPressed = y; changed = true; }

  if (g) greenSeen  = true;
  if (r) redSeen    = true;
  if (y) yellowSeen = true;

  if (changed) {
    if (g) {
      Serial.println("[BTN] GREEN  pressed  (GPIO 9)");
      // debug RFID on green press
      if (rfidOk) {
        // write-verify test
        rc522WriteReg(REG_TxMode, 0xAA);
        byte rb = rc522ReadReg(REG_TxMode);
        rc522WriteReg(REG_TxMode, 0x00);  // restore
        Serial.printf("[RFID] Write test: wrote 0xAA, read 0x%02X %s\n",
          rb, rb == 0xAA ? "OK" : "FAIL - WRITES BROKEN");

        // dump key registers
        Serial.printf("[RFID] Command=0x%02X TxControl=0x%02X TxASK=0x%02X Mode=0x%02X\n",
          rc522ReadReg(REG_Command), rc522ReadReg(REG_TxControl),
          rc522ReadReg(REG_TxASK), rc522ReadReg(REG_Mode));
        Serial.printf("[RFID] TMode=0x%02X TPre=0x%02X TRH=0x%02X TRL=0x%02X\n",
          rc522ReadReg(REG_TModeReg), rc522ReadReg(REG_TPrescaler),
          rc522ReadReg(REG_TReloadH), rc522ReadReg(REG_TReloadL));

        Serial.println("[RFID] Debug scan (hold card near reader)...");
        byte atqa[2];
        rc522ClearBitMask(REG_Coll, 0x80);
        byte cmd = PICC_WUPA;
        byte recvLen = 2;
        byte validBits = 0x07;
        byte result = rc522Transceive(&cmd, 1, atqa, &recvLen, &validBits, true);
        Serial.printf("[RFID] WUPA result=%d recvLen=%d\n", result, recvLen);
        if (result == 0) {
          Serial.printf("[RFID] ATQA: %02X %02X\n", atqa[0], atqa[1]);
          // try anti-collision
          byte uid[5];
          byte anticollCmd[2] = { PICC_ANTICOLL, 0x20 };
          recvLen = 5;
          result = rc522Transceive(anticollCmd, 2, uid, &recvLen, NULL, true);
          Serial.printf("[RFID] AntiColl result=%d recvLen=%d\n", result, recvLen);
          if (result == 0 && recvLen >= 4) {
            Serial.printf("[RFID] UID: %02X:%02X:%02X:%02X\n", uid[0], uid[1], uid[2], uid[3]);
          }
        }
      }
    }
    if (r) Serial.println("[BTN] RED    pressed  (GPIO 13)");
    if (y) Serial.println("[BTN] YELLOW pressed  (GPIO 10)");
  }

  // scan for RFID cards every 300ms
  if (rfidOk && millis() - lastRfidCheck > 300) {
    lastRfidCheck = millis();

    byte atqa[2];
    if (rc522RequestCard(atqa, true)) {  // true = WUPA (wakes halted cards too)
      byte uid[5];
      if (rc522AntiCollision(uid)) {
        String uidStr = uidToString(uid, 4);

        // debounce: ignore same card within 1.5s
        if (uidStr != lastScannedUid || millis() - lastScanTime > 1500) {
          lastScannedUid = uidStr;
          lastScanTime = millis();

          Serial.println("--------------------------------");
          Serial.printf("[RFID] Card detected! UID: %s\n", uidStr.c_str());
          Serial.printf("       ATQA: %02X %02X\n", atqa[0], atqa[1]);
          Serial.println("--------------------------------");

          changed = true;
        }

        rc522HaltCard();
      }
    }
  }

  // refresh display at ~10Hz or on change
  if (changed || millis() - lastRefresh > 100) {
    drawScreen();
    lastRefresh = millis();
  }

  delay(20);
}

// ══════════════════════════════════════════════════════════════════════
//  DRAW SCREEN
// ══════════════════════════════════════════════════════════════════════
void drawScreen() {
  int y = 10;
  int lineH = 24;

  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(20, y);
  tft.print("  HW TEST v1  ");
  y += lineH + 8;

  tft.drawFastHLine(10, y, 220, TFT_DARKGREY);
  y += 8;

  tft.setTextSize(2);
  drawStatus("DISPLAY", displayOk, y); y += lineH;
  drawStatus("IMU    ", imuOk,     y); y += lineH;
  drawStatus("RC522  ", rfidOk,    y); y += lineH;

  y += 4;
  tft.drawFastHLine(10, y, 220, TFT_DARKGREY);
  y += 10;

  tft.setTextSize(2);
  drawButton("GREEN ", greenPressed,  greenSeen,  TFT_GREEN,  y); y += lineH;
  drawButton("RED   ", redPressed,    redSeen,    TFT_RED,    y); y += lineH;
  drawButton("YELLOW", yellowPressed, yellowSeen, TFT_YELLOW, y); y += lineH;

  // RFID last scan
  y += 4;
  tft.drawFastHLine(10, y, 220, TFT_DARKGREY);
  y += 10;

  tft.setTextSize(2);
  if (lastScannedUid.length() > 0) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, y);
    tft.print("LAST SCAN:      ");
    y += lineH;
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(10, y);
    tft.print(lastScannedUid);
    tft.print("        ");
  } else if (rfidOk) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(10, y);
    tft.print("Tap a card...   ");
    y += lineH;
    tft.setCursor(10, y);
    tft.print("                ");
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(10, y);
    tft.print("RFID offline    ");
    y += lineH;
    tft.setCursor(10, y);
    tft.print("                ");
  }
}

void drawStatus(const char* label, bool ok, int y) {
  tft.setCursor(10, y);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.print(label);
  tft.print(" ");
  if (ok) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.print("PASS");
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.print("FAIL");
  }
  tft.print("  ");
}

void drawButton(const char* label, bool pressed, bool seen, uint16_t color, int y) {
  tft.setCursor(10, y);
  tft.setTextColor(color, TFT_BLACK);
  tft.print(label);
  tft.print(" ");

  if (pressed) {
    tft.setTextColor(TFT_BLACK, color);
    tft.print("HELD");
  } else if (seen) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.print(" OK ");
  } else {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.print("----");
  }
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.print("  ");
}
