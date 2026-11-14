#include <Arduino.h>

#include "esp_system.h"

#if DIAG_STAGE >= 2
#include <USB.h>
#include <USBHIDKeyboard.h>
#endif

#if DIAG_STAGE >= 3
#include "driver/gpio.h"
#endif

#if DIAG_STAGE >= 4
#include "driver/rmt.h"
#endif

namespace {

constexpr gpio_num_t kFiveVoltEnablePin = GPIO_NUM_14;
RTC_DATA_ATTR uint32_t bootCount = 0;

#if DIAG_STAGE >= 2
USBHIDKeyboard keyboard;

void usbEvent(void*, esp_event_base_t base, int32_t id, void*) {
  if (base == ARDUINO_USB_EVENTS) {
    Serial.printf("USB event=%ld uptime=%lu\n", static_cast<long>(id),
                  static_cast<unsigned long>(millis()));
  }
}
#endif

#if DIAG_STAGE >= 4
constexpr size_t kKeyLedCount = 6;
constexpr size_t kAmbientLedCount = 8;
constexpr size_t kBitsPerPixel = 24;
constexpr uint8_t kClockDivider = 2;
rmt_item32_t keyItems[kKeyLedCount * kBitsPerPixel];
rmt_item32_t ambientItems[kAmbientLedCount * kBitsPerPixel];

struct Timing {
  uint16_t zeroHigh;
  uint16_t zeroLow;
  uint16_t oneHigh;
  uint16_t oneLow;
};

constexpr Timing kKeyTiming = {16, 34, 32, 18};
constexpr Timing kAmbientTiming = {12, 24, 24, 12};

bool initRmt(gpio_num_t pin, rmt_channel_t channel) {
  rmt_config_t config = RMT_DEFAULT_CONFIG_TX(pin, channel);
  config.clk_div = kClockDivider;
  config.tx_config.loop_en = false;
  config.tx_config.carrier_en = false;
  config.tx_config.idle_output_en = true;
  config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
  return rmt_config(&config) == ESP_OK &&
         rmt_driver_install(channel, 0, 0) == ESP_OK;
}

void appendByte(uint8_t value, size_t& index, const Timing& timing,
                rmt_item32_t* items) {
  for (int bit = 7; bit >= 0; --bit) {
    const bool one = (value & (1U << bit)) != 0;
    items[index++] = {{{
        one ? timing.oneHigh : timing.zeroHigh, 1,
        one ? timing.oneLow : timing.zeroLow, 0,
    }}};
  }
}

bool sendSolid(rmt_channel_t channel, size_t count, uint8_t red,
               uint8_t green, uint8_t blue, const Timing& timing,
               rmt_item32_t* items) {
  size_t index = 0;
  for (size_t pixel = 0; pixel < count; ++pixel) {
    appendByte(green, index, timing, items);
    appendByte(red, index, timing, items);
    appendByte(blue, index, timing, items);
  }
  return rmt_write_items(channel, items, index, true) == ESP_OK;
}
#endif

}  // namespace

void setup() {
  ++bootCount;
  Serial.begin(115200);
  delay(300);
  Serial.printf("DIAG boot=%lu stage=%d reset_reason=%d cpu=%luMHz heap=%u psram=%u\n",
                static_cast<unsigned long>(bootCount), DIAG_STAGE,
                static_cast<int>(esp_reset_reason()),
                static_cast<unsigned long>(getCpuFrequencyMhz()),
                ESP.getFreeHeap(), ESP.getFreePsram());

#ifdef USB_LINE_TEST
  pinMode(19, INPUT_PULLUP);
  pinMode(20, INPUT_PULLUP);
  delay(50);
  Serial.println("DIAG USB line test: GPIO19=D-, GPIO20=D+");
#endif

#if DIAG_STAGE >= 2
  USB.VID(0x303A);
  USB.PID(0x8361);
  USB.manufacturerName("reCodex diagnostics");
  USB.productName("reCodex USB isolation test");
  USB.onEvent(usbEvent);
  keyboard.begin();
  Serial.printf("DIAG usb_begin=%d\n", USB.begin() ? 1 : 0);
#endif

#if DIAG_STAGE >= 3
  gpio_set_direction(kFiveVoltEnablePin, GPIO_MODE_OUTPUT);
  gpio_set_level(kFiveVoltEnablePin, 1);
  Serial.println("DIAG GPIO14 LED power enabled; LED data pins untouched");
#endif

#if DIAG_STAGE >= 4
  const bool keyReady = initRmt(GPIO_NUM_12, RMT_CHANNEL_0);
  const bool ambientReady = initRmt(GPIO_NUM_13, RMT_CHANNEL_1);
  const bool keySent = keyReady && sendSolid(
      RMT_CHANNEL_0, kKeyLedCount, 0, 0, 0, kKeyTiming, keyItems);
  const bool ambientSent = ambientReady && sendSolid(
      RMT_CHANNEL_1, kAmbientLedCount, 0, 32, 0,
      kAmbientTiming, ambientItems);
  Serial.printf("DIAG leds key=%d ambient=%d; ambient current limited green\n",
                keySent ? 1 : 0, ambientSent ? 1 : 0);
#endif
}

void loop() {
  static uint32_t lastHeartbeat = 0;
  const uint32_t now = millis();
  if (now - lastHeartbeat >= 1000) {
    lastHeartbeat = now;
    Serial.printf("DIAG heartbeat stage=%d uptime=%lu heap=%u usb=%d\n",
                  DIAG_STAGE, static_cast<unsigned long>(now),
                  ESP.getFreeHeap(),
#if DIAG_STAGE >= 2
                  static_cast<bool>(USB) ? 1 : 0
#else
                  0
#endif
    );
#ifdef USB_LINE_TEST
    Serial.printf("DIAG usb_lines dminus=%d dplus=%d\n",
                  digitalRead(19), digitalRead(20));
#endif
  }
  delay(10);
}
