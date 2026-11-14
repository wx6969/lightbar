// SPDX-License-Identifier: MIT
// Copyright (c) 2026 imliubo

#pragma once

#ifndef CODEX_BLE_ENABLED
#define CODEX_BLE_ENABLED 0
#endif

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BLECharacteristic.h>
#include <BLEHIDDevice.h>
#include <BLEServer.h>
#include <USB.h>

#include <array>

#include "ControlOnlyUsbHid.h"
#include "HidOtaUpdate.h"

struct ThreadLight {
  uint32_t color = 0;
  float brightness = 0.0f;
  uint8_t effect = 0;
  float speed = 0.0f;
  bool syncKeysLighting = false;
  bool syncAmbientLighting = false;
};

struct LightingSide {
  uint32_t color = 0;
  float brightness = 0.0f;
  uint8_t effect = 0;
  float speed = 0.0f;
  float magic = 0.0f;
};

struct CodexMicroState {
  std::array<ThreadLight, 6> threads;
  LightingSide ambient;
  LightingSide keys;
  int8_t ambientSyncThread = -1;
  bool connected = false;
  bool lightingReady = false;
  bool dirty = true;
  uint32_t threadsRevision = 0;
  uint32_t ambientRevision = 0;
};

class CodexMicroBle {
 public:
  static constexpr uint16_t kVendorId = 0x303A;
  static constexpr uint16_t kProductId = 0x8360;
  static constexpr uint8_t kReportId = 6;

  void begin(bool clearBleBonds = false);
  void poll();
  void setBattery(uint8_t percentage, bool charging);
  void sendKey(const char* key, uint8_t action, int8_t agent = -1);
  void sendJoystick(float angle, float distance);
  bool connected();
  CodexMicroState snapshot();

 private:
  class ServerCallbacks;
  class InputCallbacks;
  class OutputCallbacks;
  void onConnected(bool connected);
  void onUsbOutput(uint8_t reportId, const uint8_t* data, size_t length);
  static void usbOutputCallback(void* context, uint8_t reportId,
                                const uint8_t* data, size_t length);
  void onUsbEvent(int32_t eventId);
  void reconcileUsbMountedState();
  static void usbEventCallback(void* arg, esp_event_base_t eventBase,
                               int32_t eventId, void* eventData);
  void applyPendingTransportChange();
  bool transportAvailable(bool usb);
  void onMtuChanged(uint16_t mtu);
  void onNotifyStatus(BLECharacteristicCallbacks::Status status,
                      uint32_t code);
  void onOutput(const uint8_t* data, size_t length, bool usb);
  void handleRpc(const JsonDocument& request);
  void sendResult(JsonVariantConst id, JsonVariantConst result);
  void sendSuccess(JsonVariantConst id);
  void sendJson(const String& json);
  void transmitJson(const String& json, bool usb);
  void processHidOta();
  void clearTransmitQueue();
  void updateThreadLighting(JsonArrayConst values);
  void updateAmbientLighting(JsonObjectConst value);

  BLEHIDDevice* hid_ = nullptr;
  BLEServer* server_ = nullptr;
  BLECharacteristic* input_ = nullptr;
  BLECharacteristic* output_ = nullptr;
  ControlOnlyUsbHid usbHid_;
  SemaphoreHandle_t stateMutex_ = nullptr;
  SemaphoreHandle_t transmitMutex_ = nullptr;
  SemaphoreHandle_t receiveMutex_ = nullptr;
  CodexMicroState state_;
  String rpcBuffer_;
  bool rpcBufferUsb_ = false;
  uint8_t batteryPercentage_ = 100;
  bool charging_ = false;
  bool bleConnected_ = false;
  bool usbMounted_ = false;
  bool usbSuspended_ = false;
  bool pendingTransportChange_ = false;
  bool usbResumeRecoveryPending_ = false;
  uint32_t usbResumeRecoveryAtMs_ = 0;
  HidOtaUpdate hidOta_;
  uint32_t otaRestartAtMs_ = 0;

  static constexpr size_t kTransmitQueueDepth = 32;
  std::array<String, kTransmitQueueDepth> transmitQueue_;
  std::array<bool, kTransmitQueueDepth> transmitQueueUsb_;
  size_t transmitHead_ = 0;
  size_t transmitTail_ = 0;
  size_t transmitCount_ = 0;
  String pendingReceiveLog_;
  uint16_t peerMtu_ = 23;
  bool pendingMtuLog_ = false;
  BLECharacteristicCallbacks::Status pendingNotifyStatus_ =
      BLECharacteristicCallbacks::Status::ERROR_NO_CLIENT;
  uint32_t pendingNotifyCode_ = 0;
  bool pendingNotifyStatusValid_ = false;

  static CodexMicroBle* usbEventOwner_;
};
