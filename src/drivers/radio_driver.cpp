#include "drivers/radio_driver.h"

#include <Arduino.h>

#include "config.h"

RadioDriver::RadioDriver()
    : radio_(new Module(LORA_CS, LORA_DIO0, LORA_RST, LORA_DIO1)) {}

int16_t RadioDriver::begin() {
  pinMode(LORA_DIO0, INPUT);
  int16_t status = radio_.begin(LORA_FREQUENCY, 125.0, 11, 5, 0x12, 10, 8);
  ready_ = status == RADIOLIB_ERR_NONE;
  return status;
}

int16_t RadioDriver::startTransmit(const uint8_t *data, size_t length) {
  return radio_.startTransmit(data, length);
}

bool RadioDriver::transmissionComplete() {
  return digitalRead(LORA_DIO0) == HIGH;
}

int16_t RadioDriver::finishTransmit() {
  return radio_.finishTransmit();
}

bool RadioDriver::ready() const {
  return ready_;
}

bool RadioDriver::statusOk(int16_t status) {
  return status == RADIOLIB_ERR_NONE;
}
