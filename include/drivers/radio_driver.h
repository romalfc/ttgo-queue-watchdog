#pragma once

#include <RadioLib.h>

class RadioDriver {
 public:
  RadioDriver();
  int16_t begin();
  int16_t startTransmit(const uint8_t *data, size_t length);
  bool transmissionComplete();
  int16_t finishTransmit();
  bool ready() const;
  static bool statusOk(int16_t status);

 private:
  SX1276 radio_;
  bool ready_ = false;
};
