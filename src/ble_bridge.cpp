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
#include <host/ble_store.h>   // ble_store_clear() — wipe NimBLE bonds on unpair
#include <Arduino.h>
#include <string.h>

#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// 8 KB: the main loop has blocking windows of hundreds of ms (screenshot
// SD write, touch-bus recovery) during which a connected host keeps
// notifying ~500 B per connection event at MTU 517 — the old 2 KB ring
// overflowed after ~4 events and silently truncated a JSON line mid-flight.
static const size_t RX_CAP = 8192;
static uint8_t  rxBuf[RX_CAP];
static volatile size_t rxHead = 0, rxTail = 0;
// rxHead is advanced by the NimBLE host task (onWrite callback); rxTail by the
// main loop (bleRead). On the dual-core ESP32-S3 those run on different cores,
// so the head/tail updates need a real lock — `volatile` alone doesn't make the
// index updates atomic across cores. portMUX is the standard ESP32 spinlock for
// task↔task shared state; the critical sections here only copy a few bytes.
static portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

static BLEServer*         server = nullptr;
static BLECharacteristic* txChar = nullptr;
static BLECharacteristic* rxChar = nullptr;
static volatile bool      connected = false;
static volatile bool      secure = false;
static volatile uint32_t  passkey = 0;
static volatile uint16_t  mtu = 23;

// Ring-full is not silent any more: the flag + counter let the drain loop
// in data.h resync on a line boundary and log how much was lost, instead
// of fusing the truncated tail with the next write into one garbled line.
static volatile bool     rxOverflow = false;
static volatile uint32_t rxDropped  = 0;

static void rxPush(const uint8_t* p, size_t n) {
  portENTER_CRITICAL(&rxMux);
  size_t i = 0;
  for (; i < n; i++) {
    size_t next = (rxHead + 1) % RX_CAP;
    if (next == rxTail) break;   // full — drop the rest
    rxBuf[rxHead] = p[i];
    rxHead = next;
  }
  if (i < n) { rxOverflow = true; rxDropped += (uint32_t)(n - i); }
  portEXIT_CRITICAL(&rxMux);
}

bool bleRxOverflowTake() {
  bool v = rxOverflow;
  if (v) rxOverflow = false;
  return v;
}

uint32_t bleRxDropped() { return rxDropped; }

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
    // Terminate any partial line so a stale fragment from this connection
    // can't fuse with the first write of the next one — the parser drops
    // the terminated fragment visibly (json err) instead.
    static const uint8_t nl = '\n';
    rxPush(&nl, 1);
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
  // setCapability is NOT optional: the NimBLE setAuthenticationMode overload
  // only sets sm_bonding/sm_mitm/sm_sc and leaves io_cap at the constructor
  // default NoInputNoOutput — which forces "Just Works" pairing regardless
  // of the MITM bit, silently skipping the passkey exchange the UI renders.
  BLESecurity* sec = new BLESecurity();
  sec->setAuthenticationMode(/*bonding*/ true, /*mitm*/ true, /*sc*/ true);
  sec->setCapability(ESP_IO_CAP_OUT);   // DisplayOnly — device shows a passkey
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
  // Wipe every persisted bond/CCCD from the NimBLE host store. Without this,
  // "unpair" / factory-reset acked success but the desktop could reconnect
  // with no fresh passkey — the bond survived in NVS. ble_store_clear() drops
  // them all; the next connection has to re-pair from scratch.
  int rc = ble_store_clear();
  Serial.printf("[ble] cleared bonds (ble_store_clear rc=%d)\n", rc);
}

size_t bleAvailable() {
  portENTER_CRITICAL(&rxMux);
  size_t a = (rxHead + RX_CAP - rxTail) % RX_CAP;
  portEXIT_CRITICAL(&rxMux);
  return a;
}

int bleRead() {
  portENTER_CRITICAL(&rxMux);
  int b = -1;
  if (rxHead != rxTail) {
    b = rxBuf[rxTail];
    rxTail = (rxTail + 1) % RX_CAP;
  }
  portEXIT_CRITICAL(&rxMux);
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
