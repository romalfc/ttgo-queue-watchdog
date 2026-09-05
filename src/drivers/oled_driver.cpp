#include "drivers/oled_driver.h"

#include <Wire.h>

#include "config.h"

OledDriver::OledDriver()
    : display_(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET) {}

bool OledDriver::begin() {
  Wire.begin(OLED_SDA, OLED_SCL);
  bool ready = display_.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (ready) {
    clear();
  }
  return ready;
}

void OledDriver::clear() {
  display_.clearDisplay();
  display_.display();
}

void OledDriver::drawCursor(bool visible) {
  display_.fillRect(122, 56, 6, 8, visible ? SSD1306_WHITE : SSD1306_BLACK);
  display_.display();
}
