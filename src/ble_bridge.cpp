// NimBLE backing for ble_bridge.h — for the ws-amoled-18 build, whose
// arduino-esp32 core 3.x ships the NimBLE BLE stack (the original
// ble_bridge.cpp is Bluedroid and is used by the m5stickc-plus build).
//
// Same Nordic UART Service + line-buffered RX/TX as the Bluedroid version;
// only the stack-specific bits (callback signatures, security setup, no
// BLE2902/encryption-level) differ. The public ble_bridge.h API is identical.
//
// Compiled only when CONFIG_NIMBLE_ENABLED so this file is inert on Bluedroid.

#include "ble_bridge.h"
#include <sdkconfig.h>

#if defined(CONFIG_NIMBLE_ENABLED) || defined(CONFIG_BT_NIMBLE_ENABLED)

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLESecurity.h>
#include <Arduino.h>
#include <string.h>

#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

static const size_t RX_CAP = 2048;
static uint8_t  rxBuf[RX_CAP];
static volatile size_t rxHead = 0, rxTail = 0;

static BLEServer*         server = nullptr;
static BLECharacteristic* txChar = nullptr;
static BLECharacteristic* rxChar = nullptr;
static volatile bool      connected = false;
static volatile bool      secure = false;
static volatile uint32_t  passkey = 0;
static volatile uint16_t  mtu = 23;

static void rxPush(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    size_t next = (rxHead + 1) % RX_CAP;
    if (next == rxTail) return;   // full — drop
    rxBuf[rxHead] = p[i];
    rxHead = next;
  }
}

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c, ble_gap_conn_desc*) override {
    String v = c->getValue();
    if (v.length()) rxPush((const uint8_t*)v.c_str(), v.length());
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    connected = true;
    Serial.println("[ble] connected");
  }
  void onDisconnect(BLEServer*) override {
    connected = false; secure = false; passkey = 0; mtu = 23;
    Serial.println("[ble] disconnected");
    BLEDevice::startAdvertising();
  }
  void onMtuChanged(BLEServer*, ble_gap_conn_desc*, uint16_t newMtu) override {
    mtu = newMtu;
    Serial.printf("[ble] mtu=%u\n", mtu);
  }
};

// DisplayOnly: the stack shows a 6-digit passkey, the desktop types it.
class SecCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  bool onConfirmPIN(uint32_t) override { return false; }
  bool onSecurityRequest() override { return true; }
  void onPassKeyNotify(uint32_t pk) override {
    passkey = pk;
    Serial.printf("[ble] passkey %06lu\n", (unsigned long)pk);
  }
  void onAuthenticationComplete(ble_gap_conn_desc*) override {
    passkey = 0;
    secure = true;
    Serial.println("[ble] auth complete");
  }
};

void bleInit(const char* deviceName) {
  BLEDevice::init(deviceName);
  BLEDevice::setMTU(517);
  BLEDevice::setSecurityCallbacks(new SecCallbacks());

  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService* svc = server->createService(NUS_SERVICE_UUID);

  // TX: notify (encrypted read). NimBLE auto-creates the CCCD — no BLE2902.
  txChar = svc->createCharacteristic(
    NUS_TX_UUID,
    BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ_ENC);

  rxChar = svc->createCharacteristic(
    NUS_RX_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR |
    BLECharacteristic::PROPERTY_WRITE_ENC);
  rxChar->setCallbacks(new RxCallbacks());

  svc->start();

  // LE Secure Connections + MITM + bonding, DisplayOnly so a passkey shows.
  BLESecurity* sec = new BLESecurity();
  sec->setAuthenticationMode(/*bonding*/ true, /*mitm*/ true, /*sc*/ true);
  sec->setKeySize(16);
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.printf("[ble] advertising as '%s'\n", deviceName);
}

bool bleConnected() { return connected; }
bool bleSecure()    { return secure; }
uint32_t blePasskey() { return passkey; }

void bleClearBonds() {
  // NimBLE clears bonds via the host store (ble_store_clear); wire that up
  // when porting factory-reset. Not needed for the pairing bring-up.
  Serial.println("[ble] bleClearBonds: not implemented on NimBLE yet");
}

size_t bleAvailable() { return (rxHead + RX_CAP - rxTail) % RX_CAP; }

int bleRead() {
  if (rxHead == rxTail) return -1;
  int b = rxBuf[rxTail];
  rxTail = (rxTail + 1) % RX_CAP;
  return b;
}

size_t bleWrite(const uint8_t* data, size_t len) {
  if (!connected || !txChar) return 0;
  size_t chunk = mtu > 3 ? mtu - 3 : 20;
  if (chunk > 180) chunk = 180;
  size_t sent = 0;
  while (sent < len) {
    size_t n = len - sent;
    if (n > chunk) n = chunk;
    txChar->setValue((uint8_t*)(data + sent), n);
    txChar->notify();
    sent += n;
    delay(4);
  }
  return sent;
}

#endif  // CONFIG_NIMBLE_ENABLED
