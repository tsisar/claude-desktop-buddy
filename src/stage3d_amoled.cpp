// Stage 3d — BLE bring-up on AMOLED. Prove the Nordic UART Service comes up
// under arduino-esp32 core 3.x: advertise as Claude-XXXX, show connection
// status + pairing passkey on screen, echo received bytes to serial. No JSON
// parsing yet (that's stage 3e). Touch is ignored here.
#include <Arduino.h>
#include <esp_mac.h>   // esp_read_mac / ESP_MAC_BT (core 3.x)
#include "board_pins.h"
#include "hal/display.h"
#include "ble_bridge.h"

static Surface gfx;
static bool dispOk = false;
static char btName[16] = "Claude";

void setup() {
  Serial.begin(115200); Serial.setTxTimeoutMs(0); delay(300);
  Serial.println("\n[stage3d] BLE bring-up");
  dispOk = gfx.begin();

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
  Serial.printf("[stage3d] advertising as %s\n", btName);
}

void loop() {
  static uint32_t next = 0;
  // drain RX, echo to serial
  while (bleAvailable()) { int c = bleRead(); if (c >= 0) Serial.write((char)c); }

  if (dispOk && millis() >= next) {
    next = millis() + 250;
    gfx.fillSprite(0x0000);
    gfx.setTextDatum(MC_DATUM);
    gfx.setTextSize(3); gfx.setTextColor(0x6B0D, 0x0000);
    gfx.drawString("BLE", LCD_WIDTH/2, 90);
    gfx.setTextSize(2); gfx.setTextColor(0xFFFF, 0x0000);
    gfx.drawString(btName, LCD_WIDTH/2, 150);
    uint32_t pk = blePasskey();
    if (pk) {
      char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)pk);
      gfx.setTextColor(0xFFE0, 0x0000); gfx.setTextSize(2);
      gfx.drawString("passkey:", LCD_WIDTH/2, 220);
      gfx.setTextSize(4); gfx.drawString(b, LCD_WIDTH/2, 270);
    } else {
      const char* st = !bleConnected() ? "advertising..."
                     : bleSecure() ? "connected (secure)" : "connecting...";
      gfx.setTextColor(bleConnected()?0x07E0:0x8410, 0x0000); gfx.setTextSize(2);
      gfx.drawString(st, LCD_WIDTH/2, 240);
    }
    gfx.setTextDatum(TL_DATUM);
    gfx.pushSprite();
  }
  delay(8);
}
