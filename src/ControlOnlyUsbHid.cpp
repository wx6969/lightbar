// SPDX-License-Identifier: MIT

#include "ControlOnlyUsbHid.h"

#include <cstring>

#include "esp32-hal-tinyusb.h"

namespace {

const uint8_t* reportDescriptor = nullptr;
uint16_t reportDescriptorLength = 0;
ControlOnlyUsbHid::OutputCallback outputCallback = nullptr;
void* outputContext = nullptr;
SemaphoreHandle_t inputComplete = nullptr;
SemaphoreHandle_t inputMutex = nullptr;
bool interfaceRegistered = false;

uint16_t loadHidDescriptor(uint8_t* destination, uint8_t* interfaceNumber) {
  if (destination == nullptr || interfaceNumber == nullptr ||
      reportDescriptor == nullptr || reportDescriptorLength == 0) {
    return 0;
  }

  const uint8_t endpoint = tinyusb_get_free_in_endpoint();
  if (endpoint == 0) {
    return 0;
  }
  const uint8_t stringIndex =
      tinyusb_add_string_descriptor("Codex Micro HID");
  const uint8_t descriptor[TUD_HID_DESC_LEN] = {
      TUD_HID_DESCRIPTOR(*interfaceNumber, stringIndex,
                         HID_ITF_PROTOCOL_NONE,
                         reportDescriptorLength,
                         static_cast<uint8_t>(0x80 | endpoint),
                         64, 1)};
  ++(*interfaceNumber);
  memcpy(destination, descriptor, sizeof(descriptor));
  return sizeof(descriptor);
}

}  // namespace

bool ControlOnlyUsbHid::begin(const uint8_t* descriptor,
                              uint16_t descriptorLength,
                              OutputCallback callback, void* context) {
  if (descriptor == nullptr || descriptorLength == 0 || callback == nullptr ||
      interfaceRegistered) {
    return false;
  }

  if (inputComplete == nullptr) {
    inputComplete = xSemaphoreCreateBinary();
  }
  if (inputMutex == nullptr) {
    inputMutex = xSemaphoreCreateMutex();
  }
  if (inputComplete == nullptr || inputMutex == nullptr) {
    return false;
  }

  reportDescriptor = descriptor;
  reportDescriptorLength = descriptorLength;
  outputCallback = callback;
  outputContext = context;
  interfaceRegistered =
      tinyusb_enable_interface(USB_INTERFACE_HID, TUD_HID_DESC_LEN,
                               loadHidDescriptor) == ESP_OK;
  return interfaceRegistered;
}

bool ControlOnlyUsbHid::sendReport(uint8_t reportId, const void* data,
                                   size_t length, uint32_t timeoutMs) {
  if (!interfaceRegistered || inputComplete == nullptr ||
      inputMutex == nullptr || data == nullptr) {
    return false;
  }

  const TickType_t timeoutTicks = pdMS_TO_TICKS(timeoutMs);
  if (xSemaphoreTake(inputMutex, timeoutTicks) != pdTRUE) {
    return false;
  }

  bool sent = tud_hid_n_ready(0);
  if (sent) {
    xSemaphoreTake(inputComplete, 0);
    sent = tud_hid_n_report(0, reportId, data, length);
    if (sent && xSemaphoreTake(inputComplete, timeoutTicks) != pdTRUE) {
      sent = false;
    }
  }

  xSemaphoreGive(inputMutex);
  return sent;
}

extern "C" uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
  (void)instance;
  return reportDescriptor;
}

extern "C" uint16_t tud_hid_get_report_cb(
    uint8_t instance, uint8_t reportId, hid_report_type_t reportType,
    uint8_t* buffer, uint16_t requestedLength) {
  (void)instance;
  (void)reportId;
  (void)reportType;
  (void)buffer;
  (void)requestedLength;
  return 0;
}

extern "C" void tud_hid_set_report_cb(
    uint8_t instance, uint8_t reportId, hid_report_type_t reportType,
    const uint8_t* buffer, uint16_t bufferSize) {
  (void)instance;
  (void)reportType;
  if (outputCallback == nullptr || buffer == nullptr) {
    return;
  }

  if (reportId == 0 && bufferSize > 0 && buffer[0] == 6) {
    outputCallback(outputContext, buffer[0], buffer + 1, bufferSize - 1);
    return;
  }
  outputCallback(outputContext, reportId, buffer, bufferSize);
}

extern "C" void tud_hid_report_complete_cb(uint8_t instance,
                                             const uint8_t* report,
                                             uint16_t length) {
  (void)instance;
  (void)report;
  (void)length;
  if (inputComplete != nullptr) {
    xSemaphoreGive(inputComplete);
  }
}
