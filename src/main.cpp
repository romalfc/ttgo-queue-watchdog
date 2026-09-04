#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_RESET (-1)
#define BUTTON_PIN 0

constexpr char SINGLE_BUTTON_COMMAND[] = "single-btn";
constexpr char DOUBLE_BUTTON_COMMAND[] = "double-btn";
constexpr char DEADLOCK_COMMAND[] = "deadlock";
constexpr uint32_t DOUBLE_CLICK_THRESHOLD_MS = 350;

constexpr uint32_t BLINK_INTERVALS_MS[] = {250, 500, 1000, 2000};
constexpr size_t BLINK_INTERVAL_COUNT = sizeof(BLINK_INTERVALS_MS) /
                                         sizeof(BLINK_INTERVALS_MS[0]);
constexpr TickType_t BUTTON_DEBOUNCE = pdMS_TO_TICKS(40);
constexpr TickType_t DOUBLE_CLICK_WINDOW = pdMS_TO_TICKS(350);

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
enum class ButtonPress : uint8_t {
  Single,
  Double
};

struct SerialButtonCommand {
  ButtonPress press;
  uint8_t repeatCount;
  char text[24];
};

struct SerialCommandResult {
  char text[24];
  uint32_t intervalMs;
};

QueueHandle_t buttonEventQueue;
QueueHandle_t blinkIntervalQueue;
QueueHandle_t serialResultQueue;
QueueHandle_t deadlockQueue;
SemaphoreHandle_t displayMutex;

void applyButtonPress(ButtonPress press, size_t &intervalIndex) {
  if (press == ButtonPress::Double) {
    intervalIndex = (intervalIndex + BLINK_INTERVAL_COUNT - 1) %
                    BLINK_INTERVAL_COUNT;
  } else {
    intervalIndex = (intervalIndex + 1) % BLINK_INTERVAL_COUNT;
  }

  uint32_t newInterval = BLINK_INTERVALS_MS[intervalIndex];
  xQueueSend(blinkIntervalQueue, &newInterval, portMAX_DELAY);
}

void buttonTask(void *parameter) {
  (void)parameter;
  bool previousState = digitalRead(BUTTON_PIN);
  size_t intervalIndex = 0;

  xQueueSend(blinkIntervalQueue, &BLINK_INTERVALS_MS[intervalIndex], portMAX_DELAY);

  for (;;) {
    SerialButtonCommand command;
    if (xQueueReceive(buttonEventQueue, &command, 0) == pdTRUE) {
      for (uint8_t pressIndex = 0; pressIndex < command.repeatCount; ++pressIndex) {
        applyButtonPress(command.press, intervalIndex);
      }

      SerialCommandResult result{};
      strncpy(result.text, command.text, sizeof(result.text) - 1);
      result.intervalMs = BLINK_INTERVALS_MS[intervalIndex];
      xQueueSend(serialResultQueue, &result, portMAX_DELAY);
    }

    bool currentState = digitalRead(BUTTON_PIN);

    if (previousState == HIGH && currentState == LOW) {
      vTaskDelay(BUTTON_DEBOUNCE);
      if (digitalRead(BUTTON_PIN) == LOW) {
        while (digitalRead(BUTTON_PIN) == LOW) {
          vTaskDelay(pdMS_TO_TICKS(10));
        }

        vTaskDelay(BUTTON_DEBOUNCE);
        bool doubleClick = false;
        TickType_t waitStarted = xTaskGetTickCount();

        while (xTaskGetTickCount() - waitStarted < DOUBLE_CLICK_WINDOW) {
          if (digitalRead(BUTTON_PIN) == LOW) {
            vTaskDelay(BUTTON_DEBOUNCE);
            if (digitalRead(BUTTON_PIN) == LOW) {
              doubleClick = true;
              while (digitalRead(BUTTON_PIN) == LOW) {
                vTaskDelay(pdMS_TO_TICKS(10));
              }
              break;
            }
          }
          vTaskDelay(pdMS_TO_TICKS(10));
        }

        applyButtonPress(doubleClick ? ButtonPress::Double : ButtonPress::Single,
                         intervalIndex);
      }
    }

    previousState = currentState;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void cursorTask(void *parameter) {
  (void)parameter;
  uint32_t blinkIntervalMs = BLINK_INTERVALS_MS[0];
  bool cursorVisible = false;

  for (;;) {
    uint32_t requestedIntervalMs;
    if (xQueueReceive(blinkIntervalQueue, &requestedIntervalMs,
                      pdMS_TO_TICKS(blinkIntervalMs)) == pdTRUE) {
      blinkIntervalMs = requestedIntervalMs;
      continue;
    }

    cursorVisible = !cursorVisible;
    xSemaphoreTake(displayMutex, portMAX_DELAY);
    display.fillRect(122, 56, 6, 8,
                     cursorVisible ? SSD1306_WHITE : SSD1306_BLACK);
    display.display();
    xSemaphoreGive(displayMutex);
  }
}

void deadlockTask(void *parameter) {
  (void)parameter;
  bool deadlockRequested;

  for (;;) {
    if (xQueueReceive(deadlockQueue, &deadlockRequested, portMAX_DELAY) == pdTRUE) {
      xSemaphoreTake(displayMutex, portMAX_DELAY);
      Serial.println(F("DeadlockTask owns displayMutex and will never release it."));
      for (;;) {
        vTaskDelay(portMAX_DELAY);
      }
    }
  }
}

void processSerialCommands() {
  static char command[24];
  static size_t commandLength = 0;

  while (Serial.available() > 0) {
    char character = static_cast<char>(Serial.read());
    if (character == '\r') {
      continue;
    }

    if (character == '\n') {
      command[commandLength] = '\0';
      unsigned long intervalMs = 0;
      bool queued = false;
      SerialButtonCommand event{};
      strncpy(event.text, command, sizeof(event.text) - 1);

      if (strcmp(command, DEADLOCK_COMMAND) == 0) {
        bool trigger = true;
        queued = xQueueSend(deadlockQueue, &trigger, 0) == pdTRUE;
        if (queued) {
          Serial.println(F("Deadlock requested; device tasks will block on displayMutex."));
        }
      } else if (strcmp(command, SINGLE_BUTTON_COMMAND) == 0) {
        event.press = ButtonPress::Single;
        event.repeatCount = 1;
        queued = xQueueSend(buttonEventQueue, &event, 0) == pdTRUE;
      } else if (strcmp(command, DOUBLE_BUTTON_COMMAND) == 0) {
        event.press = ButtonPress::Double;
        event.repeatCount = 1;
        queued = xQueueSend(buttonEventQueue, &event, 0) == pdTRUE;
      } else if (sscanf(command, "double-btn %lu", &intervalMs) == 1 &&
                 intervalMs >= 100 && intervalMs <= 2000) {
        if (intervalMs > DOUBLE_CLICK_THRESHOLD_MS) {
          event.press = ButtonPress::Single;
          event.repeatCount = 2;
        } else {
          event.press = ButtonPress::Double;
          event.repeatCount = 1;
        }
        queued = xQueueSend(buttonEventQueue, &event, 0) == pdTRUE;
      }

      if (!queued && commandLength > 0) {
        Serial.println(F("Unknown command or full button queue"));
      }
      commandLength = 0;
      continue;
    }

    if (commandLength < sizeof(command) - 1) {
      command[commandLength++] = character;
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.display();

  buttonEventQueue = xQueueCreate(8, sizeof(SerialButtonCommand));
  blinkIntervalQueue = xQueueCreate(2, sizeof(uint32_t));
  serialResultQueue = xQueueCreate(8, sizeof(SerialCommandResult));
    deadlockQueue = xQueueCreate(1, sizeof(bool));
    displayMutex = xSemaphoreCreateMutex();
  if (buttonEventQueue == nullptr || blinkIntervalQueue == nullptr ||
      serialResultQueue == nullptr || deadlockQueue == nullptr ||
      displayMutex == nullptr) {
    while (true) {
      delay(1000);
    }
  }

  xTaskCreatePinnedToCore(buttonTask, "ButtonTask", 2048, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(cursorTask, "CursorTask", 2048, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(deadlockTask, "DeadlockTask", 2048, nullptr, 1, nullptr, 0);
}

void loop() {
  processSerialCommands();

  xSemaphoreTake(displayMutex, portMAX_DELAY);
  SerialCommandResult result;
  while (xQueueReceive(serialResultQueue, &result, 0) == pdTRUE) {
    Serial.printf("Command: %s; cursor blink interval: %lu ms\n",
                  result.text, static_cast<unsigned long>(result.intervalMs));
  }
  xSemaphoreGive(displayMutex);

  vTaskDelay(pdMS_TO_TICKS(10));
}