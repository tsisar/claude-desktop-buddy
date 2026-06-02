#include "imu.h"
#include "../board_pins.h"
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <SensorQMI8658.hpp>

// QMI8658-backed IMU HAL. See imu.h for the why.

static SensorQMI8658 imu;
static bool          ok = false;

bool imuInit(TwoWire& w) {
  if (ok) return true;
  // SensorLib's begin() re-runs Wire.begin internally with these pins; the
  // shared bus is already up by the time we get called, so this is a no-op
  // on the wire and just attaches the driver. Pass the raw addr (0x6B).
  if (!imu.begin(w, IMU_ADDR, IIC_SDA, IIC_SCL)) {
    Serial.printf("[imu] QMI8658 not found at 0x%02X\n", IMU_ADDR);
    return false;
  }
  // ±8g range covers shake (delta > 0.8g) and face-down detection with
  // headroom. 1000Hz ODR is plenty — we sample at most 20Hz from the loop.
  imu.configAccelerometer(SensorQMI8658::ACC_RANGE_8G,
                          SensorQMI8658::ACC_ODR_1000Hz,
                          SensorQMI8658::LPF_MODE_0);
  imu.enableAccelerometer();
  ok = true;
  Serial.println("[imu] QMI8658 ok");
  return true;
}

bool imuOk() { return ok; }

bool imuGetAccel(float* ax, float* ay, float* az) {
  if (!ok) return false;
  float x, y, z;
  if (!imu.getAccelerometer(x, y, z)) return false;
  // SensorLib's getAccelerometer() returns g-units already (raw int16 *
  // (range/32768)), NOT m/s². No extra scaling needed.
  //
  // Z-axis sign flip: on this board the QMI8658 sits with its z+ pointing
  // INTO the screen — sitting face-up on a table yields z ≈ -1g. We invert
  // here so the HAL matches the M5 convention (face-up = +1g), which lets
  // the original face-down detector (az < -0.7) port unchanged.
  *ax = x;
  *ay = y;
  *az = -z;
  return true;
}

float imuTemp() {
  if (!ok) return NAN;
  return imu.getTemperature_C();
}