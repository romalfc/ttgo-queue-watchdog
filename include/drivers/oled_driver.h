#pragma once

#include <Adafruit_SSD1306.h>

class OledDriver {
 public:
  OledDriver();
  bool begin();
  void clear();
  void drawCursor(bool visible);

 private:
  Adafruit_SSD1306 display_;
};
