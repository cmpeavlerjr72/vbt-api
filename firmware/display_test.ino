/*
  display_test.ino — DIAGNOSTIC VERSION
  Prints the full TFT_eSPI config so we can see exactly what pins/driver it's using.
*/

#include <TFT_eSPI.h>
#include <SPI.h>

TFT_eSPI tft = TFT_eSPI(240, 240);

void setup() {
  Serial.begin(115200);
  delay(2000);  // extra time for serial monitor to connect
  Serial.println("\n\n========== DISPLAY DIAGNOSTIC ==========");

  // Print the full TFT_eSPI setup BEFORE calling tft.begin()
  setup_t tftSetup;
  tft.getSetup(tftSetup);

  Serial.print("Driver:    "); Serial.println(tftSetup.tft_driver, HEX);
  Serial.print("Width:     "); Serial.println(tftSetup.tft_width);
  Serial.print("Height:    "); Serial.println(tftSetup.tft_height);
  Serial.print("SPI port:  "); Serial.println(tftSetup.port);
  Serial.print("MOSI pin:  "); Serial.println(tftSetup.pin_tft_mosi);
  Serial.print("MISO pin:  "); Serial.println(tftSetup.pin_tft_miso);
  Serial.print("SCLK pin:  "); Serial.println(tftSetup.pin_tft_clk);
  Serial.print("CS pin:    "); Serial.println(tftSetup.pin_tft_cs);
  Serial.print("DC pin:    "); Serial.println(tftSetup.pin_tft_dc);
  Serial.print("RST pin:   "); Serial.println(tftSetup.pin_tft_rst);
  Serial.print("BL pin:    "); Serial.println(tftSetup.pin_tft_led);

  Serial.println("========================================\n");

  // Now initialize
  tft.begin();
  tft.setRotation(2);

  Serial.println("tft.begin() done.");

  // Color cycle test
  tft.fillScreen(TFT_RED);
  Serial.println("RED");
  delay(1000);

  tft.fillScreen(TFT_GREEN);
  Serial.println("GREEN");
  delay(1000);

  tft.fillScreen(TFT_BLUE);
  Serial.println("BLUE");
  delay(1000);

  tft.fillScreen(TFT_WHITE);
  Serial.println("WHITE");
  delay(1000);

  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(3);
  tft.setTextColor(TFT_GREEN);
  tft.setCursor(30, 100);
  tft.println("HELLO");

  Serial.println("Test complete.");
}

void loop() {
  delay(1000);
}
