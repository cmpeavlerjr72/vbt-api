/*
  rfid_pin_test.ino
  Quick test: MFRC522 library with our actual wired pins.
  Serial only — no display, no conflicts.

  Pin Map (as soldered):
    RC522 SS   -> GPIO 4
    RC522 SCK  -> GPIO 5
    RC522 MOSI -> GPIO 3
    RC522 MISO -> GPIO 7
    RC522 RST  -> GPIO 14
    RC522 3.3V -> 3V3
    RC522 GND  -> GND
*/

#include <SPI.h>
#include <MFRC522.h>

#define SS_PIN   4
#define RST_PIN  14

MFRC522 mfrc522(SS_PIN, RST_PIN);

String lastUid = "";
unsigned long lastScanMs = 0;

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("=== RFID PIN TEST ===");
  Serial.println("SPI.begin(SCK=5, MISO=7, MOSI=3, SS=4)");

  SPI.begin(5, 7, 3, SS_PIN);

  Serial.println("PCD_Init...");
  mfrc522.PCD_Init();
  delay(50);

  mfrc522.PCD_DumpVersionToSerial();

  Serial.println();
  Serial.println("Ready. Hold wristband near reader...");
}

void loop() {
  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  String uid = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (i > 0) uid += ":";
    if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(mfrc522.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  unsigned long now = millis();
  if (uid == lastUid && (now - lastScanMs) < 1500) {
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    return;
  }

  lastUid = uid;
  lastScanMs = now;

  Serial.println("--------------------------------");
  Serial.printf("UID: %s\n", uid.c_str());
  Serial.println("--------------------------------");

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  delay(200);
}
