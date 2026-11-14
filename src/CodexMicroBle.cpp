// SPDX-License-Identifier: MIT
// Copyright (c) 2026 imliubo

#include "CodexMicroBle.h"

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <BLEUtils.h>
#include <USB.h>
#include <tusb.h>
#include <vector>

#include "esp_gap_ble_api.h"

namespace {

// These identity values and the HID descriptor are copied unchanged from
// codex-micro-4-core2 commit 2ee23a4ab696f94bb78d250f28cc4a9b879ba079.
constexpr char kDeviceName[] = "Codex Micro";
constexpr char kManufacturer[] = "Work Louder";
constexpr char kFirmwareVersion[] = "0.1.12-direct5v";
constexpr char kUsbSerial[] = "2884851ECB78";
constexpr size_t kPayloadSize = 61;
constexpr size_t kReportBodySize = 63;
constexpr uint16_t kRequiredMtu = kReportBodySize + 3;
constexpr uint16_t kLocalMtu = 517;
constexpr uint16_t kUsbFirmwareVersion = 0x0111;
constexpr uint32_t kUsbReportTimeoutMs = 20;
constexpr uint32_t kUsbResumeRecoveryDelayMs = 120;

constexpr uint16_t swapBytes(uint16_t value) {
  return static_cast<uint16_t>((value << 8) | (value >> 8));
}

float clampUnit(float value) {
  if (value < 0.0f) {
    return 0.0f;
  }
  if (value > 1.0f) {
    return 1.0f;
  }
  return value;
}

// One vendor-defined input/output report. HIDAPI adds/removes Report ID 6,
// while the BLE characteristics carry the remaining 63-byte report body.
const uint8_t kReportMap[] = {
    0x06, 0x00, 0xFF,        // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,              // Usage (1)
    0xA1, 0x01,              // Collection (Application)
    0x85, 0x06,              // Report ID (6)
    0x15, 0x00,              // Logical Minimum (0)
    0x26, 0xFF, 0x00,        // Logical Maximum (255)
    0x75, 0x08,              // Report Size (8)
    0x95, 0x3F,              // Report Count (63)
    0x09, 0x01,              // Usage (1)
    0x81, 0x02,              // Input (Data, Variable, Absolute)
    0x95, 0x3F,              // Report Count (63)
    0x09, 0x02,              // Usage (2)
    0x91, 0x02,              // Output (Data, Variable, Absolute)
    0xC0                     // End Collection
};

class SecurityCallbacks final : public BLESecurityCallbacks {
 public:
  bool onSecurityRequest() override { return true; }
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t) override {}
  bool onConfirmPIN(uint32_t) override { return true; }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t result) override {
    Serial.printf("BLE pairing %s\n", result.success ? "complete" : "failed");
  }
};

class SemaphoreGuard {
 public:
  explicit SemaphoreGuard(SemaphoreHandle_t semaphore)
      : semaphore_(semaphore) {
    if (semaphore_ != nullptr) {
      xSemaphoreTake(semaphore_, portMAX_DELAY);
    }
  }

  ~SemaphoreGuard() {
    if (semaphore_ != nullptr) {
      xSemaphoreGive(semaphore_);
    }
  }

  SemaphoreGuard(const SemaphoreGuard&) = delete;
  SemaphoreGuard& operator=(const SemaphoreGuard&) = delete;

 private:
  SemaphoreHandle_t semaphore_;
};

bool clearStoredBleBonds() {
  const int bondCount = esp_ble_get_bond_device_num();
  if (bondCount < 0) {
    Serial.println("ERR BLE bond count unavailable");
    return false;
  }
  if (bondCount == 0) {
    Serial.println("BLE bond reset requested; no stored bonds found");
    return true;
  }

  std::vector<esp_ble_bond_dev_t> bonds(
      static_cast<size_t>(bondCount));
  int listedCount = bondCount;
  const esp_err_t listResult =
      esp_ble_get_bond_device_list(&listedCount, bonds.data());
  if (listResult != ESP_OK) {
    Serial.printf("ERR BLE bond list result=%d\n",
                  static_cast<int>(listResult));
    return false;
  }

  bool allRequestsSucceeded = true;
  for (int index = 0; index < listedCount; ++index) {
    const int countBeforeRemoval = esp_ble_get_bond_device_num();
    const esp_err_t removeResult =
        esp_ble_remove_bond_device(bonds[index].bd_addr);
    if (removeResult != ESP_OK) {
      allRequestsSucceeded = false;
      Serial.printf("ERR BLE bond remove index=%d result=%d\n", index,
                    static_cast<int>(removeResult));
      continue;
    }

    const uint32_t removalStartedMs = millis();
    while (millis() - removalStartedMs < 1000) {
      const int remaining = esp_ble_get_bond_device_num();
      if (remaining >= 0 && remaining < countBeforeRemoval) {
        break;
      }
      delay(10);
    }
  }

  const int remaining = esp_ble_get_bond_device_num();
  const bool cleared = allRequestsSucceeded && remaining == 0;
  Serial.printf("BLE bond reset %s removed=%d remaining=%d\n",
                cleared ? "complete" : "incomplete",
                bondCount - max(remaining, 0), remaining);
  return cleared;
}

}  // namespace

CodexMicroBle* CodexMicroBle::usbEventOwner_ = nullptr;

class CodexMicroBle::ServerCallbacks final : public BLEServerCallbacks {
 public:
  explicit ServerCallbacks(CodexMicroBle& owner) : owner_(owner) {}

  void onConnect(BLEServer*) override { owner_.onConnected(true); }

  void onDisconnect(BLEServer*) override {
    owner_.onConnected(false);
  }

  void onMtuChanged(BLEServer*,
                    esp_ble_gatts_cb_param_t* param) override {
    owner_.onMtuChanged(param->mtu.mtu);
  }

 private:
  CodexMicroBle& owner_;
};

class CodexMicroBle::InputCallbacks final
    : public BLECharacteristicCallbacks {
 public:
  explicit InputCallbacks(CodexMicroBle& owner) : owner_(owner) {}

  void onStatus(BLECharacteristic*,
                BLECharacteristicCallbacks::Status status,
                uint32_t code) override {
    owner_.onNotifyStatus(status, code);
  }

 private:
  CodexMicroBle& owner_;
};

class CodexMicroBle::OutputCallbacks final
    : public BLECharacteristicCallbacks {
 public:
  explicit OutputCallbacks(CodexMicroBle& owner) : owner_(owner) {}

  void onWrite(BLECharacteristic* characteristic) override {
    const std::string value = characteristic->getValue();
    owner_.onOutput(reinterpret_cast<const uint8_t*>(value.data()),
                    value.size(), false);
  }

 private:
  CodexMicroBle& owner_;
};

void CodexMicroBle::begin(bool clearBleBonds) {
  stateMutex_ = xSemaphoreCreateMutex();
  transmitMutex_ = xSemaphoreCreateMutex();
  receiveMutex_ = xSemaphoreCreateMutex();
  hidOta_.begin();
  Serial.printf("FIRMWARE %s built %s %s\n", kFirmwareVersion, __DATE__, __TIME__);

#if CODEX_BLE_ENABLED
  BLEDevice::init(kDeviceName);
  if (clearBleBonds) {
    clearStoredBleBonds();
  }
  const esp_err_t mtuResult = BLEDevice::setMTU(kLocalMtu);
  BLEDevice::setSecurityCallbacks(new SecurityCallbacks());

  auto* security = new BLESecurity();
  security->setCapability(ESP_IO_CAP_NONE);
  security->setAuthenticationMode(ESP_LE_AUTH_BOND);

  server_ = BLEDevice::createServer();
  server_->setCallbacks(new ServerCallbacks(*this));

  hid_ = new BLEHIDDevice(server_);
  hid_->manufacturer()->setValue(kManufacturer);
  // Low release bits mark the transport as wireless in the desktop bridge.
  // Arduino-ESP32 2.x serializes these fields big-endian, while the BLE PnP
  // characteristic is little-endian. Pre-swap so macOS enumerates 303A:8360.
  hid_->pnp(0x02, swapBytes(kVendorId), swapBytes(kProductId),
            swapBytes(0x0101));
  hid_->hidInfo(0x00, 0x01);
  hid_->reportMap(const_cast<uint8_t*>(kReportMap), sizeof(kReportMap));

  input_ = hid_->inputReport(kReportId);
  output_ = hid_->outputReport(kReportId);
  input_->setCallbacks(new InputCallbacks(*this));
  output_->setCallbacks(new OutputCallbacks(*this));
  hid_->startServices();
  hid_->setBatteryLevel(batteryPercentage_);

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->setAppearance(GENERIC_HID);
  advertising->addServiceUUID(hid_->hidService()->getUUID());
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
#else
  (void)clearBleBonds;
#endif

  usbEventOwner_ = this;
  USB.VID(kVendorId);
  USB.PID(kProductId);
  USB.firmwareVersion(kUsbFirmwareVersion);
  USB.manufacturerName(kManufacturer);
  USB.productName(kDeviceName);
  USB.serialNumber(kUsbSerial);
  // The board is powered exclusively from USB VBUS. Arduino-ESP32 defaults
  // to self-powered, which can make Windows apply an incompatible D3 policy.
  USB.usbAttributes(0);
  USB.usbPower(500);
  USB.onEvent(usbEventCallback);
  const bool hidStarted =
      usbHid_.begin(kReportMap, sizeof(kReportMap), usbOutputCallback, this);
  const bool usbStarted = hidStarted && USB.begin();

#if CODEX_BLE_ENABLED
  Serial.printf(
      "BLE vendor HID ready VID=%04X PID=%04X usage=FF00 report=%u\n",
      kVendorId, kProductId, kReportId);
  Serial.printf("BLE local MTU=%u result=%d required=%u\n",
                kLocalMtu, static_cast<int>(mtuResult), kRequiredMtu);
#else
  Serial.println(
      "BLE disabled at compile time; controller and radio not initialized");
#endif
  Serial.printf(
      "USB vendor HID stack %s VID=%04X PID=%04X usage=FF00 report=%u\n",
      usbStarted ? "ready" : "failed", kVendorId, kProductId,
      kReportId);
  Serial.printf("USB serial=%s transport=control-out\n", kUsbSerial);
  Serial.println(
      "USB HID OTA ready (inactive OTA slot, CRC32 + SHA-256)");
}

void CodexMicroBle::poll() {
  if (transmitMutex_ == nullptr) {
    return;
  }

  reconcileUsbMountedState();
  applyPendingTransportChange();
  if (usbResumeRecoveryPending_ &&
      static_cast<int32_t>(millis() - usbResumeRecoveryAtMs_) >= 0) {
    usbResumeRecoveryPending_ = false;
    if (tud_mounted() && !tud_suspended()) {
      Serial.println("USB resume recovery: forcing HID re-enumeration");
      if (tud_disconnect()) {
        delay(60);
        tud_connect();
      } else {
        Serial.println("USB resume recovery: disconnect unsupported");
      }
    }
  }
  processHidOta();
  if (otaRestartAtMs_ != 0 &&
      static_cast<int32_t>(millis() - otaRestartAtMs_) >= 0) {
    Serial.println("HID OTA restarting into verified firmware");
    Serial.flush();
    ESP.restart();
  }
  if (hidOta_.active()) {
    return;
  }

  String json;
  bool jsonUsb = false;
  String receivedJson;
  uint16_t mtu = 23;
  bool printMtu = false;
  BLECharacteristicCallbacks::Status notifyStatus =
      BLECharacteristicCallbacks::Status::ERROR_NO_CLIENT;
  uint32_t notifyCode = 0;
  bool printNotifyStatus = false;
  xSemaphoreTake(transmitMutex_, portMAX_DELAY);
  if (!pendingReceiveLog_.isEmpty()) {
    receivedJson = pendingReceiveLog_;
    pendingReceiveLog_.clear();
  }
  if (transmitCount_ > 0) {
    json = transmitQueue_[transmitTail_];
    jsonUsb = transmitQueueUsb_[transmitTail_];
    transmitQueue_[transmitTail_].clear();
    transmitTail_ = (transmitTail_ + 1) % kTransmitQueueDepth;
    --transmitCount_;
  }
  mtu = peerMtu_;
  printMtu = pendingMtuLog_;
  pendingMtuLog_ = false;
  if (pendingNotifyStatusValid_) {
    notifyStatus = pendingNotifyStatus_;
    notifyCode = pendingNotifyCode_;
    pendingNotifyStatusValid_ = false;
    printNotifyStatus = true;
  }
  xSemaphoreGive(transmitMutex_);

  if (printMtu) {
    Serial.printf("BLE peer MTU=%u (report requires >=%u)\n",
                  mtu, kRequiredMtu);
  }
  if (printNotifyStatus) {
    const char* name = "UNKNOWN";
    switch (notifyStatus) {
      case BLECharacteristicCallbacks::Status::SUCCESS_NOTIFY:
        name = "SUCCESS_NOTIFY";
        break;
      case BLECharacteristicCallbacks::Status::ERROR_NOTIFY_DISABLED:
        name = "ERROR_NOTIFY_DISABLED";
        break;
      case BLECharacteristicCallbacks::Status::ERROR_GATT:
        name = "ERROR_GATT";
        break;
      case BLECharacteristicCallbacks::Status::ERROR_NO_CLIENT:
        name = "ERROR_NO_CLIENT";
        break;
      default:
        break;
    }
    Serial.printf("BLE input notify status=%s code=%lu peer_mtu=%u\n",
                  name, static_cast<unsigned long>(notifyCode), mtu);
  }
  if (!receivedJson.isEmpty()) {
    Serial.print("HID RX JSON ");
    Serial.println(receivedJson);
  }
  if (!json.isEmpty()) {
    if (!transportAvailable(jsonUsb)) {
      Serial.printf("HID TX dropped after transport switch (%s)\n",
                    jsonUsb ? "USB" : "BLE");
      return;
    }
    Serial.print("HID TX JSON ");
    Serial.println(json);
    transmitJson(json, jsonUsb);
  }
}

void CodexMicroBle::setBattery(uint8_t percentage, bool charging) {
  batteryPercentage_ = constrain(percentage, 0, 100);
  charging_ = charging;
  bool updateBleBattery = false;
  if (stateMutex_ != nullptr) {
    SemaphoreGuard guard(stateMutex_);
    updateBleBattery = bleConnected_;
  }
  if (hid_ != nullptr && updateBleBattery) {
    hid_->setBatteryLevel(batteryPercentage_);
  }
}

void CodexMicroBle::sendKey(const char* key, uint8_t action, int8_t agent) {
  StaticJsonDocument<192> message;
  message["method"] = "v.oai.hid";
  JsonObject params = message.createNestedObject("params");
  params["k"] = key;
  params["act"] = action;
  if (agent >= 0) {
    params["ag"] = agent;
  }

  String json;
  serializeJson(message, json);
  sendJson(json);
  Serial.printf("HID key=%s action=%u\n", key, action);
}

void CodexMicroBle::sendJoystick(float angle, float distance) {
  StaticJsonDocument<160> message;
  message["method"] = "v.oai.rad";
  JsonObject params = message.createNestedObject("params");
  params["a"] = angle;
  params["d"] = distance;

  String json;
  serializeJson(message, json);
  sendJson(json);
}

bool CodexMicroBle::connected() {
  if (stateMutex_ == nullptr) {
    return false;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const bool result = state_.connected;
  xSemaphoreGive(stateMutex_);
  return result;
}

CodexMicroState CodexMicroBle::snapshot() {
  CodexMicroState copy;
  if (stateMutex_ == nullptr) {
    return copy;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  copy = state_;
  state_.dirty = false;
  xSemaphoreGive(stateMutex_);
  return copy;
}

void CodexMicroBle::onConnected(bool connected) {
  bool clearQueue = false;
  {
    SemaphoreGuard guard(stateMutex_);
    bleConnected_ = connected;
    state_.connected = usbMounted_ || bleConnected_;
    if (!usbMounted_) {
      state_.lightingReady = false;
    }
    state_.dirty = true;
    pendingTransportChange_ =
        (connected && usbMounted_) || (!connected && !usbMounted_);
    clearQueue = !connected && !usbMounted_;
  }
  if (!connected && receiveMutex_ != nullptr) {
    SemaphoreGuard guard(receiveMutex_);
    if (!rpcBuffer_.isEmpty() && !rpcBufferUsb_) {
      rpcBuffer_.clear();
    }
  }
  if (clearQueue) {
    clearTransmitQueue();
  }
  Serial.printf("BLE host %s\n",
                connected ? "connected" : "disconnected");
}

void CodexMicroBle::onUsbOutput(uint8_t reportId, const uint8_t* data,
                                size_t length) {
  // Some host paths pass the report ID separately, while others include it
  // as the first byte of the payload. Accept both forms, including the
  // report-id-zero callback used by TinyUSB for raw OUT transfers.
  if (reportId == 0 && length > 0 && data != nullptr &&
      data[0] == kReportId) {
    reportId = kReportId;
    ++data;
    --length;
  }
  if (reportId != kReportId) {
    return;
  }
  const size_t offset =
      (length > 0 && data[0] == kReportId) ? 1 : 0;
  if (length > offset && data[offset] == HidOtaUpdate::kRequestType) {
    hidOta_.enqueue(data + offset, length - offset);
    return;
  }
  onOutput(data, length, true);
}

void CodexMicroBle::usbOutputCallback(void* context, uint8_t reportId,
                                      const uint8_t* data, size_t length) {
  if (context != nullptr) {
    static_cast<CodexMicroBle*>(context)->onUsbOutput(reportId, data, length);
  }
}

void CodexMicroBle::usbEventCallback(void*, esp_event_base_t eventBase,
                                     int32_t eventId, void*) {
  if (usbEventOwner_ != nullptr &&
      eventBase == ARDUINO_USB_EVENTS) {
    usbEventOwner_->onUsbEvent(eventId);
  }
}

void CodexMicroBle::onUsbEvent(int32_t eventId) {
  const char* eventName = nullptr;
  {
    SemaphoreGuard guard(stateMutex_);
    switch (eventId) {
      case ARDUINO_USB_STARTED_EVENT:
        usbMounted_ = true;
        usbSuspended_ = false;
        state_.lightingReady = false;
        pendingTransportChange_ = true;
        eventName = "mounted";
        break;
      case ARDUINO_USB_STOPPED_EVENT:
        usbMounted_ = false;
        usbSuspended_ = false;
        state_.lightingReady = false;
        pendingTransportChange_ = true;
        eventName = "unmounted";
        break;
      case ARDUINO_USB_SUSPEND_EVENT:
        usbSuspended_ = true;
        state_.lightingReady = false;
        pendingTransportChange_ = true;
        eventName = "suspended";
        break;
      case ARDUINO_USB_RESUME_EVENT:
        usbSuspended_ = false;
        state_.lightingReady = false;
        pendingTransportChange_ = true;
        usbResumeRecoveryPending_ = true;
        usbResumeRecoveryAtMs_ = millis() + kUsbResumeRecoveryDelayMs;
        eventName = "resumed";
        break;
      default:
        return;
    }
    state_.connected = (usbMounted_ && !usbSuspended_) || bleConnected_;
    state_.dirty = true;
  }
  Serial.printf("USB host %s\n", eventName);
}

void CodexMicroBle::reconcileUsbMountedState() {
  // Read the stack directly: Arduino's callback cache can outlive a bus reset.
  const bool mounted = tud_mounted();
  const bool suspended = tud_suspended();
  {
    SemaphoreGuard guard(stateMutex_);
    if (usbMounted_ == mounted && usbSuspended_ == suspended) {
      return;
    }

    const bool wasUsbAvailable = usbMounted_ && !usbSuspended_;
    const bool wasUsbSuspended = usbSuspended_;
    const bool usbAvailable = mounted && !suspended;

    // Recover if an asynchronous Arduino USB mount/unmount event was missed
    // or reached the application out of order.
    usbMounted_ = mounted;
    usbSuspended_ = suspended;
    if (wasUsbAvailable != usbAvailable) {
      state_.lightingReady = false;
    }
    if (wasUsbSuspended && usbAvailable) {
      usbResumeRecoveryPending_ = true;
      usbResumeRecoveryAtMs_ = millis() + kUsbResumeRecoveryDelayMs;
    }
    pendingTransportChange_ = true;
    state_.connected = (usbMounted_ && !usbSuspended_) || bleConnected_;
    state_.dirty = true;
  }
  Serial.printf("USB bus mounted=%u suspended=%u\n", mounted, suspended);
}

void CodexMicroBle::applyPendingTransportChange() {
  bool pending = false;
  bool useUsb = false;
  bool bleConnected = false;
  {
    SemaphoreGuard guard(stateMutex_);
    pending = pendingTransportChange_;
    if (pending) {
      pendingTransportChange_ = false;
      useUsb = usbMounted_ && !usbSuspended_;
      bleConnected = bleConnected_;
    }
  }
  if (!pending) {
    return;
  }

  if (!useUsb) {
    hidOta_.onUsbDisconnected();
    clearTransmitQueue();
  }

  if (receiveMutex_ != nullptr) {
    SemaphoreGuard guard(receiveMutex_);
    if (!rpcBuffer_.isEmpty() && rpcBufferUsb_ != useUsb) {
      rpcBuffer_.clear();
    }
  }

#if CODEX_BLE_ENABLED
  if (useUsb) {
    BLEDevice::getAdvertising()->stop();
    if (bleConnected && server_ != nullptr) {
      server_->disconnect(server_->getConnId());
    }
    Serial.println(
        "TRANSPORT USB selected (host enumeration succeeded)");
  } else {
    if (!bleConnected) {
      BLEDevice::startAdvertising();
    }
    Serial.println(
        "TRANSPORT BLE selected (USB host not enumerated)");
  }
#else
  if (useUsb) {
    Serial.println(
        "TRANSPORT USB selected (BLE disabled at compile time)");
  } else {
    Serial.println(
        "TRANSPORT unavailable (USB not enumerated; BLE disabled)");
  }
#endif
}

bool CodexMicroBle::transportAvailable(bool usb) {
  if (stateMutex_ == nullptr) {
    return false;
  }
  SemaphoreGuard guard(stateMutex_);
  return usb ? (usbMounted_ && !usbSuspended_)
             : (!usbMounted_ && bleConnected_);
}

void CodexMicroBle::onMtuChanged(uint16_t mtu) {
  if (transmitMutex_ == nullptr) {
    return;
  }
  xSemaphoreTake(transmitMutex_, portMAX_DELAY);
  peerMtu_ = mtu;
  pendingMtuLog_ = true;
  xSemaphoreGive(transmitMutex_);
}

void CodexMicroBle::onNotifyStatus(
    BLECharacteristicCallbacks::Status status, uint32_t code) {
  if (transmitMutex_ == nullptr) {
    return;
  }
  xSemaphoreTake(transmitMutex_, portMAX_DELAY);
  pendingNotifyStatus_ = status;
  pendingNotifyCode_ = code;
  pendingNotifyStatusValid_ = true;
  xSemaphoreGive(transmitMutex_);
}

void CodexMicroBle::onOutput(const uint8_t* data, size_t length,
                             bool usb) {
  if (data == nullptr || length < 2) {
    return;
  }
  if (!transportAvailable(usb)) {
    return;
  }
  if (hidOta_.active()) {
    return;
  }
  SemaphoreGuard receiveGuard(receiveMutex_);
  if (!rpcBuffer_.isEmpty() && rpcBufferUsb_ != usb) {
    rpcBuffer_.clear();
  }
  if (rpcBuffer_.isEmpty()) {
    rpcBufferUsb_ = usb;
  }

  // HOGP normally strips the report ID. Accept an included ID as well so the
  // transport remains compatible with hosts that forward the raw report.
  size_t offset = (length >= 3 && data[0] == kReportId) ? 1 : 0;
  if (length < offset + 2 || data[offset] != 2) {
    return;
  }

  const size_t payloadLength =
      min<size_t>(data[offset + 1], kPayloadSize);
  if (length < offset + 2 + payloadLength) {
    return;
  }
  const char* payload =
      reinterpret_cast<const char*>(data + offset + 2);
  constexpr char kTopLevelPrefix[] = "{\"method\"";
  const bool startsTopLevel =
      payloadLength >= sizeof(kTopLevelPrefix) - 1 &&
      memcmp(payload, kTopLevelPrefix,
             sizeof(kTopLevelPrefix) - 1) == 0;
  if (startsTopLevel && !rpcBuffer_.isEmpty()) {
    // A new top-level object means a previous fragmented write was dropped.
    // Resynchronize immediately instead of poisoning the next request.
    rpcBuffer_.clear();
  }
  if (rpcBuffer_.isEmpty()) {
    size_t jsonStart = 0;
    while (jsonStart < payloadLength && payload[jsonStart] != '{') {
      ++jsonStart;
    }
    if (jsonStart == payloadLength) {
      return;
    }
    rpcBuffer_.concat(payload + jsonStart, payloadLength - jsonStart);
  } else {
    rpcBuffer_.concat(payload, payloadLength);
  }

  DynamicJsonDocument request(4096);
  const DeserializationError error =
      deserializeJson(request, rpcBuffer_);
  if (error == DeserializationError::IncompleteInput) {
    return;
  }
  if (error) {
    Serial.printf("RPC parse error: %s\n", error.c_str());
    rpcBuffer_.clear();
    return;
  }

  if (transmitMutex_ != nullptr) {
    xSemaphoreTake(transmitMutex_, portMAX_DELAY);
    pendingReceiveLog_ = rpcBuffer_;
    xSemaphoreGive(transmitMutex_);
  }
  handleRpc(request);
  rpcBuffer_.clear();
}

void CodexMicroBle::handleRpc(const JsonDocument& request) {
  const char* method = request["method"] | "";
  JsonVariantConst id = request["id"];
  JsonVariantConst params = request["params"];
  Serial.printf("RPC method=%s\n", method);

  if (strcmp(method, "sys.version") == 0) {
    StaticJsonDocument<128> resultDoc;
    resultDoc["version"] = kFirmwareVersion;
    sendResult(id, resultDoc.as<JsonVariantConst>());
    return;
  }

  if (strcmp(method, "device.status") == 0) {
    StaticJsonDocument<256> resultDoc;
    resultDoc["version"] = kFirmwareVersion;
    resultDoc["profile_index"] = 0;
    resultDoc["layer_index"] = 1;
    resultDoc["battery"] = batteryPercentage_;
    resultDoc["is_charging"] = charging_;
    sendResult(id, resultDoc.as<JsonVariantConst>());
    return;
  }

  if (strcmp(method, "v.oai.thstatus") == 0) {
    if (params.is<JsonArrayConst>()) {
      updateThreadLighting(params.as<JsonArrayConst>());
    }
    sendSuccess(id);
    return;
  }

  if (strcmp(method, "v.oai.rgbcfg") == 0) {
    if (params.is<JsonObjectConst>()) {
      const JsonObjectConst config = params.as<JsonObjectConst>();
      updateAmbientLighting(config["ambient"].as<JsonObjectConst>());
      // "keys" is the original device's single keyboard-backlight zone.
      // This board has no matching zone: its six key LEDs are Agent 0..5 and
      // are controlled only by v.oai.thstatus.
    }
    sendSuccess(id);
    return;
  }

  // The reference acknowledges these methods without applying local state.
  if (strcmp(method, "lights.preview") == 0 ||
      strcmp(method, "host.focused_app") == 0) {
    sendSuccess(id);
    return;
  }

  // This hardware build intentionally ignores unsupported host data. A valid
  // request is acknowledged so the desktop side does not see a method error.
  sendSuccess(id);
}

void CodexMicroBle::sendResult(JsonVariantConst id,
                               JsonVariantConst result) {
  DynamicJsonDocument response(512);
  response["id"] = id;
  response["result"] = result;
  String json;
  serializeJson(response, json);
  sendJson(json);
}

void CodexMicroBle::sendSuccess(JsonVariantConst id) {
  StaticJsonDocument<96> resultDoc;
  resultDoc["ok"] = true;
  sendResult(id, resultDoc.as<JsonVariantConst>());
}

void CodexMicroBle::sendJson(const String& json) {
  if (transmitMutex_ == nullptr || hidOta_.active()) {
    return;
  }
  const bool useUsb = transportAvailable(true);
  const bool useBle = !useUsb && transportAvailable(false);
  if (!useUsb && !useBle) {
    return;
  }

  xSemaphoreTake(transmitMutex_, portMAX_DELAY);
  if (transmitCount_ == kTransmitQueueDepth) {
    // Preserve forward progress if a host floods requests: discard the oldest
    // unsent response and retain the most recent request acknowledgements.
    transmitQueue_[transmitTail_].clear();
    transmitTail_ = (transmitTail_ + 1) % kTransmitQueueDepth;
    --transmitCount_;
  }
  transmitQueue_[transmitHead_] = json;
  transmitQueueUsb_[transmitHead_] = useUsb;
  transmitHead_ = (transmitHead_ + 1) % kTransmitQueueDepth;
  ++transmitCount_;
  xSemaphoreGive(transmitMutex_);
}

void CodexMicroBle::processHidOta() {
  uint8_t response[HidOtaUpdate::kReportBodySize] = {};
  if (!hidOta_.process(response)) {
    return;
  }

  const bool rebootAfterResponse =
      response[2] ==
          static_cast<uint8_t>(HidOtaUpdate::Command::kEnd) &&
      response[3] == static_cast<uint8_t>(HidOtaUpdate::Status::kOk) &&
      hidOta_.takeRebootRequest();
  const bool sent =
      transportAvailable(true) &&
      usbHid_.sendReport(kReportId, response, sizeof(response),
                         kUsbReportTimeoutMs);
  if (!sent) {
    Serial.println("HID OTA response send failed; host may retry");
    if (rebootAfterResponse) {
      // The boot partition has already been switched after full verification.
      // Reboot even if the final acknowledgement was lost.
      otaRestartAtMs_ = millis() + 1000;
    }
    return;
  }

  if (response[2] ==
          static_cast<uint8_t>(HidOtaUpdate::Command::kBegin) &&
      response[3] == static_cast<uint8_t>(HidOtaUpdate::Status::kOk) &&
      hidOta_.active()) {
    clearTransmitQueue();
  }

  if (response[2] ==
          static_cast<uint8_t>(HidOtaUpdate::Command::kData) &&
      response[3] == static_cast<uint8_t>(HidOtaUpdate::Status::kOk)) {
    const uint8_t progress = hidOta_.progressPercentage();
    static uint8_t lastLoggedProgress = 255;
    if (lastLoggedProgress == 255 || progress >= lastLoggedProgress + 10) {
      Serial.printf("HID OTA progress=%u%%\n", progress);
      lastLoggedProgress = progress;
    }
  }
  if (rebootAfterResponse) {
    otaRestartAtMs_ = millis() + 250;
  }
}

void CodexMicroBle::transmitJson(const String& json, bool usb) {
  if (!transportAvailable(usb)) {
    return;
  }

  if (!usb) {
    if (input_ == nullptr) {
      return;
    }
    // Windows persists the CCCD subscription for a bonded HID device and may
    // not rewrite it after the ESP32 reboots. Arduino-ESP32 does not persist
    // that descriptor, so its local guard otherwise drops every input report.
    // Receiving an RPC output report proves the bonded HID host is actively
    // using this channel; restore the cached notification state before reply.
    auto* notificationDescriptor = static_cast<BLE2902*>(
        input_->getDescriptorByUUID(
            BLEUUID(static_cast<uint16_t>(0x2902))));
    if (notificationDescriptor != nullptr &&
        !notificationDescriptor->getNotifications()) {
      notificationDescriptor->setNotifications(true);
      Serial.println("BLE input notification state restored");
    }
  }

  String framed = json;
  framed += '\n';
  size_t offset = 0;
  while (offset < framed.length()) {
    const size_t chunk =
        min<size_t>(kPayloadSize, framed.length() - offset);
    uint8_t report[kReportBodySize] = {};
    report[0] = 2;
    report[1] = chunk;
    memcpy(report + 2, framed.c_str() + offset, chunk);
    if (usb) {
      const bool sent =
          usbHid_.sendReport(kReportId, report, sizeof(report),
                             kUsbReportTimeoutMs);
      Serial.printf("USB input report bytes=%u result=%s\n",
                    static_cast<unsigned>(sizeof(report)),
                    sent ? "ok" : "failed");
      if (!sent) {
        return;
      }
    } else {
      Serial.printf("BLE input report bytes=%u peer_mtu=%u\n",
                    static_cast<unsigned>(sizeof(report)), peerMtu_);
      input_->setValue(report, sizeof(report));
      input_->notify();
      delay(4);
    }
    offset += chunk;
  }
}

void CodexMicroBle::clearTransmitQueue() {
  if (transmitMutex_ == nullptr) {
    return;
  }
  xSemaphoreTake(transmitMutex_, portMAX_DELAY);
  for (String& queued : transmitQueue_) {
    queued.clear();
  }
  transmitQueueUsb_.fill(false);
  transmitHead_ = 0;
  transmitTail_ = 0;
  transmitCount_ = 0;
  pendingReceiveLog_.clear();
  xSemaphoreGive(transmitMutex_);
}

void CodexMicroBle::updateThreadLighting(JsonArrayConst values) {
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  bool changed = false;
  for (JsonObjectConst value : values) {
    const int id = value["id"] | -1;
    if (id < 0 ||
        id >= static_cast<int>(state_.threads.size())) {
      continue;
    }
    ThreadLight& light = state_.threads[id];
    if (value.containsKey("c")) {
      light.color = value["c"].as<uint32_t>() & 0x00FFFFFFu;
    }
    if (value.containsKey("b")) {
      light.brightness = clampUnit(value["b"].as<float>());
    }
    if (value.containsKey("e")) {
      const int effect = value["e"].as<int>();
      if (effect >= 0 && effect <= 6) {
        light.effect = static_cast<uint8_t>(effect);
      }
    }
    if (value.containsKey("s")) {
      light.speed = clampUnit(value["s"].as<float>());
    }
    if (value.containsKey("sk")) {
      light.syncKeysLighting = value["sk"].as<int>() != 0;
    }
    if (value.containsKey("sa")) {
      const bool followAmbient = value["sa"].as<int>() != 0;
      light.syncAmbientLighting = followAmbient;
      if (followAmbient) {
        for (size_t other = 0; other < state_.threads.size(); ++other) {
          if (other != static_cast<size_t>(id)) {
            state_.threads[other].syncAmbientLighting = false;
          }
        }
        state_.ambientSyncThread = static_cast<int8_t>(id);
      } else if (state_.ambientSyncThread == id) {
        state_.ambientSyncThread = -1;
      }
    }
    changed = true;
  }
  if (changed) {
    state_.lightingReady = true;
    ++state_.threadsRevision;
    state_.dirty = true;
  }
  xSemaphoreGive(stateMutex_);
}

void CodexMicroBle::updateAmbientLighting(JsonObjectConst value) {
  if (value.isNull()) {
    return;
  }

  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  bool changed = false;
  if (value.containsKey("c")) {
    state_.ambient.color =
        value["c"].as<uint32_t>() & 0x00FFFFFFu;
    changed = true;
  }
  if (value.containsKey("b")) {
    state_.ambient.brightness =
        clampUnit(value["b"].as<float>());
    changed = true;
  }
  if (value.containsKey("e")) {
    const int effect = value["e"].as<int>();
    if (effect >= 0 && effect <= 6) {
      state_.ambient.effect = static_cast<uint8_t>(effect);
      changed = true;
    }
  }
  if (value.containsKey("s")) {
    state_.ambient.speed = clampUnit(value["s"].as<float>());
    changed = true;
  }
  if (value.containsKey("m")) {
    state_.ambient.magic = value["m"].as<float>();
    changed = true;
  }
  if (changed) {
    state_.lightingReady = true;
    ++state_.ambientRevision;
    state_.dirty = true;
  }
  xSemaphoreGive(stateMutex_);
}
