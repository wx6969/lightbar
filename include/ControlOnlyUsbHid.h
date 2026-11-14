// SPDX-License-Identifier: MIT

#pragma once

#include <Arduino.h>

class ControlOnlyUsbHid {
 public:
  using OutputCallback = void (*)(void* context, uint8_t reportId,
                                  const uint8_t* data, size_t length);

  bool begin(const uint8_t* reportDescriptor,
             uint16_t reportDescriptorLength,
             OutputCallback outputCallback, void* outputContext);
  bool sendReport(uint8_t reportId, const void* data, size_t length,
                  uint32_t timeoutMs = 100);
};
