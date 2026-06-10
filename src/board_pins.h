#pragma once
// Waveshare ESP32-S3-Touch-AMOLED-1.8 pin map.
//
// Verbatim from the official engineering-sample repo
//   waveshareteam/ESP32-S3-Touch-AMOLED-1.8
//   examples/Arduino-v3.3.5/libraries/Mylibrary/pin_config.h
// Do not "fix" these from random forum posts — an early hit listed
// CS=9/CLK=10, which is for a different board and is wrong here.

// --- AMOLED SH8601 over QSPI ---
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK    11
#define LCD_CS      12
#define LCD_WIDTH   368
#define LCD_HEIGHT  448
// SH8601 RST is not broken out to a GPIO on this board: pass
// GFX_NOT_DEFINED to Arduino_SH8601 and let gfx->begin() handle init.

// --- Shared I2C bus: FT3168 touch, QMI8658 IMU, PCF85063 RTC, AXP2101 PMU ---
#define IIC_SDA     15
#define IIC_SCL     14
#define TP_INT      21        // FT3168 touch interrupt

#define TOUCH_ADDR  0x38      // FT3168
#define IMU_ADDR    0x6B      // QMI8658
#define RTC_ADDR    0x51      // PCF85063
#define PMU_ADDR    0x34      // AXP2101

// --- BOOT button (kept as an emergency wake/back key; no A/B buttons) ---
#define BOOT_BTN    0

// --- ES8311 audio codec + speaker (hal/audio.cpp — beeps/UI feedback) ---
#define I2S_MCK_IO  16
#define I2S_BCK_IO  9
#define I2S_WS_IO   45
#define I2S_DO_IO   8
#define I2S_DI_IO   10
#define AUDIO_PA_EN 46

// --- microSD (1-bit SDMMC; hal/storage.cpp mounts it, screenshots/packs) ---
#define SDMMC_CLK   2
#define SDMMC_CMD   1
#define SDMMC_DATA  3
