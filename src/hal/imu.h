#pragma once
#include <stdint.h>
class TwoWire;

// QMI8658 IMU HAL for the Waveshare ESP32-S3-Touch-AMOLED-1.8.
//
// Mirrors the slice of the M5StickC Plus M5.Imu API the firmware actually
// uses: an init step on the shared I2C bus, plus an accelerometer read in
// g-units. The M5 build calls `M5.Imu.getAccelData(&ax,&ay,&az)` returning
// g; we keep the same units so the shake / face-down logic in main.cpp
// ports verbatim.
//
// Backed by lewisxhe/SensorLib's SensorQMI8658, which reports m/s² — we
// convert internally so callers don't have to know.

// Bring up the QMI8658 on the already-initialised `w` (shared bus, SDA=15
// SCL=14). Returns false if the chip is missing. Safe to call multiple
// times — re-inits are a no-op once `imuOk()` is true.
bool imuInit(TwoWire& w);
bool imuOk();

// Read accelerometer in g-units. Returns false if the read failed (e.g.
// chip not initialised, I2C error); in that case the outputs are untouched.
bool imuGetAccel(float* ax, float* ay, float* az);

// Die temperature in °C. Returns NaN if the read failed.
float imuTemp();