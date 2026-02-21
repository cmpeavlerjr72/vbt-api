/*
  rfid_only_test.ino
  Minimal RC522 bit-bang test — NO display, NO TFT_eSPI.
  Full card scanning via software SPI.
*/

#define RFID_SS    4
#define RFID_SCK   5
#define RFID_MOSI  2
#define RFID_MISO  7
#define RFID_RST  14

// RC522 registers
#define REG_Command     0x01
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

#define CMD_Idle        0x00
#define CMD_Transceive  0x0C

#define PICC_WUPA       0x52
#define PICC_ANTICOLL   0x93
#define PICC_HLTA       0x50

// ── Software SPI ──
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

byte rc522Read(byte reg) {
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

void rc522Write(byte reg, byte val) {
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

void rc522SetBits(byte reg, byte mask) {
  rc522Write(reg, rc522Read(reg) | mask);
}

void rc522ClearBits(byte reg, byte mask) {
  rc522Write(reg, rc522Read(reg) & ~mask);
}

// ── RC522 Init ──
void rc522Init() {
  rc522Write(REG_TxMode, 0x00);
  rc522Write(REG_RxMode, 0x00);
  rc522Write(REG_ModWidth, 0x26);
  rc522Write(REG_TModeReg, 0x80);
  rc522Write(REG_TPrescaler, 0xA9);
  rc522Write(REG_TReloadH, 0x03);
  rc522Write(REG_TReloadL, 0xE8);
  rc522Write(REG_TxASK, 0x40);
  rc522Write(REG_Mode, 0x3D);
  rc522SetBits(REG_TxControl, 0x03);
}

// ── Transceive ──
byte rc522Transceive(byte *sendData, byte sendLen, byte *recvData, byte *recvLen, byte *validBits) {
  byte txLastBits = validBits ? *validBits : 0;

  rc522Write(REG_Command, CMD_Idle);
  rc522Write(REG_ComIrq, 0x7F);
  rc522Write(REG_FIFOLevel, 0x80);

  for (byte i = 0; i < sendLen; i++) {
    rc522Write(REG_FIFOData, sendData[i]);
  }

  rc522Write(REG_BitFraming, txLastBits);
  rc522Write(REG_Command, CMD_Transceive);
  rc522SetBits(REG_BitFraming, 0x80);

  unsigned long deadline = millis() + 40;
  bool completed = false;
  byte irq = 0;
  do {
    irq = rc522Read(REG_ComIrq);
    if (irq & 0x30) { completed = true; break; }
    if (irq & 0x01) return 1;  // timeout
  } while (millis() < deadline);

  rc522ClearBits(REG_BitFraming, 0x80);
  if (!completed) return 1;

  byte errorReg = rc522Read(REG_Error);
  if (errorReg & 0x13) return 2;

  byte n = rc522Read(REG_FIFOLevel);
  if (recvLen) {
    byte maxRead = *recvLen;
    *recvLen = n;
    if (n > maxRead) n = maxRead;
  }
  for (byte i = 0; i < n; i++) {
    recvData[i] = rc522Read(REG_FIFOData);
  }
  return 0;
}

bool rc522RequestCard(byte *atqa) {
  rc522ClearBits(REG_Coll, 0x80);
  byte cmd = PICC_WUPA;
  byte recvLen = 2;
  byte validBits = 0x07;
  byte result = rc522Transceive(&cmd, 1, atqa, &recvLen, &validBits);
  return (result == 0 && recvLen == 2);
}

bool rc522AntiCollision(byte *uid) {
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

// ── State ──
String lastUid = "";
unsigned long lastScanTime = 0;
unsigned long lastCheck = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=== RFID-ONLY CARD SCAN TEST ===");
  Serial.println();

  pinMode(RFID_SS,   OUTPUT);
  pinMode(RFID_SCK,  OUTPUT);
  pinMode(RFID_MOSI, OUTPUT);
  pinMode(RFID_MISO, INPUT);
  pinMode(RFID_RST,  OUTPUT);

  digitalWrite(RFID_SS,  HIGH);
  digitalWrite(RFID_SCK, LOW);
  digitalWrite(RFID_MOSI, LOW);

  // hard reset
  digitalWrite(RFID_RST, LOW);
  delay(50);
  digitalWrite(RFID_RST, HIGH);
  delay(50);

  byte ver = rc522Read(REG_Version);
  Serial.printf("VersionReg = 0x%02X\n", ver);

  // verify writes work (ModeReg has all writable bits)
  rc522Write(REG_Mode, 0x3D);
  byte modeRb = rc522Read(REG_Mode);
  Serial.printf("Write verify: Mode=0x%02X %s\n", modeRb,
    modeRb == 0x3D ? "OK" : "FAIL");

  rc522Init();

  // verify init took effect
  byte txCtrl = rc522Read(REG_TxControl);
  byte tmode = rc522Read(REG_TModeReg);
  byte cmd = rc522Read(REG_Command);
  Serial.printf("After init: TxControl=0x%02X TMode=0x%02X Command=0x%02X\n",
    txCtrl, tmode, cmd);

  Serial.println();
  Serial.println("Scanning for cards... hold wristband near reader");
  Serial.println();
}

void loop() {
  if (millis() - lastCheck < 300) return;
  lastCheck = millis();

  byte atqa[2];
  if (rc522RequestCard(atqa)) {
    byte uid[5];
    if (rc522AntiCollision(uid)) {
      String uidStr = "";
      for (byte i = 0; i < 4; i++) {
        if (i > 0) uidStr += ":";
        if (uid[i] < 0x10) uidStr += "0";
        uidStr += String(uid[i], HEX);
      }
      uidStr.toUpperCase();

      if (uidStr != lastUid || millis() - lastScanTime > 1500) {
        lastUid = uidStr;
        lastScanTime = millis();
        Serial.printf("CARD DETECTED! UID: %s  ATQA: %02X %02X\n",
          uidStr.c_str(), atqa[0], atqa[1]);
      }
      rc522HaltCard();
    }
  }
}
