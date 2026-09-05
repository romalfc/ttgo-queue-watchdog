#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RadioLib.h>
#include <esp_system.h>
#include <stdarg.h>

#ifndef BUILD_HASH
#define BUILD_HASH "unknown"
#endif

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_RESET (-1)
#define BUTTON_PIN 0
#define LORA_CS 18
#define LORA_DIO0 26
#define LORA_DIO1 33
#define LORA_RST 23
#define LORA_FREQUENCY 868.0
#ifndef LOGGER_SERIAL_ENABLED
#define LOGGER_SERIAL_ENABLED 1
#endif

constexpr char SINGLE_BUTTON_COMMAND[] = "single-btn";
constexpr char DOUBLE_BUTTON_COMMAND[] = "double-btn";
constexpr char DEADLOCK_COMMAND[] = "deadlock";
constexpr char RADIO_COMMAND[] = "radio";
constexpr uint32_t DOUBLE_CLICK_THRESHOLD_MS = 350;
constexpr uint32_t WATCHDOG_TIMEOUT_MS = 5000;
constexpr uint32_t WATCHDOG_INCIDENT_MAGIC = 0x57444331;
constexpr size_t LORA_PACKET_SIZE = 32;
constexpr uint8_t LORA_DUTY_CYCLE_PERCENT = 1;
constexpr size_t LOG_CAPACITY = 32;
constexpr size_t LOG_MESSAGE_SIZE = 96;

constexpr uint32_t BLINK_INTERVALS_MS[] = {100, 500, 1000, 2000};
constexpr size_t BLINK_INTERVAL_COUNT = sizeof(BLINK_INTERVALS_MS) /
                                         sizeof(BLINK_INTERVALS_MS[0]);
constexpr TickType_t BUTTON_DEBOUNCE = pdMS_TO_TICKS(40);
constexpr TickType_t DOUBLE_CLICK_WINDOW = pdMS_TO_TICKS(350);

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
SX1276 radio = new Module(LORA_CS, LORA_DIO0, LORA_RST, LORA_DIO1);
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

struct WatchdogIncident {
  uint32_t magic;
  uint32_t eventUptimeMs;
  uint32_t lastCursorHeartbeatMs;
  uint32_t freeHeap;
  uint32_t minimumFreeHeap;
  uint32_t resetReason;
  uint32_t watchdogCore;
  char reason[48];
};

enum class LogLevel : uint8_t {
  Error = 0,
  Warn,
  Info,
  Debug
};

constexpr LogLevel LOGGER_DEFAULT_LEVEL = LogLevel::Info;

struct LogEntry {
  uint32_t uptimeMs;
  LogLevel level;
  char message[LOG_MESSAGE_SIZE];
};

QueueHandle_t buttonEventQueue;
QueueHandle_t blinkIntervalQueue;
QueueHandle_t serialResultQueue;
QueueHandle_t deadlockQueue;
QueueHandle_t radioTransmitQueue;
SemaphoreHandle_t displayMutex;
SemaphoreHandle_t logMutex;
volatile uint32_t cursorHeartbeatMs = 0;
RTC_DATA_ATTR WatchdogIncident lastWatchdogIncident{};
LogEntry logRing[LOG_CAPACITY]{};
size_t logRingNext = 0;
size_t logRingCount = 0;
LogLevel currentLogLevel = LOGGER_DEFAULT_LEVEL;
bool serialLoggingEnabled = LOGGER_SERIAL_ENABLED != 0;

const char *logLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Error: return "ERROR";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Info: return "INFO";
    case LogLevel::Debug: return "DEBUG";
  }
  return "UNKNOWN";
}

void logMessage(LogLevel level, const char *format, ...) {
  LogEntry entry{};
  entry.uptimeMs = millis();
  entry.level = level;
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(entry.message, sizeof(entry.message), format, arguments);
  va_end(arguments);

  if (logMutex != nullptr) {
    xSemaphoreTake(logMutex, portMAX_DELAY);
  }
  logRing[logRingNext] = entry;
  logRingNext = (logRingNext + 1) % LOG_CAPACITY;
  if (logRingCount < LOG_CAPACITY) {
    ++logRingCount;
  }
  if (serialLoggingEnabled && level <= currentLogLevel) {
    Serial.printf("[%lu ms] [%s] %s\n",
                  static_cast<unsigned long>(entry.uptimeMs),
                  logLevelName(level), entry.message);
  }
  if (logMutex != nullptr) {
    xSemaphoreGive(logMutex);
  }
}

void printVersion() {
  xSemaphoreTake(logMutex, portMAX_DELAY);
  Serial.println(F("Build information:"));
  Serial.printf("  hash=%s\n", BUILD_HASH);
  Serial.println(F("  platform=ESP32 TTGO LoRa32"));
  Serial.println(F("  framework=Arduino/FreeRTOS"));
  Serial.println(F("  radio=LoRa 868 MHz, SF11, BW125 kHz, CR5, 32 bytes"));
  Serial.printf("  duty_cycle=%u%%\n", static_cast<unsigned>(LORA_DUTY_CYCLE_PERCENT));
  Serial.printf("  log_level=%s\n", logLevelName(currentLogLevel));
  Serial.printf("  serial_logging=%s\n", serialLoggingEnabled ? "on" : "off");
  Serial.printf("  ring_entries=%u/%u\n",
                static_cast<unsigned>(logRingCount),
                static_cast<unsigned>(LOG_CAPACITY));
  Serial.println(F("Ring log:"));
  size_t firstEntry = (logRingNext + LOG_CAPACITY - logRingCount) % LOG_CAPACITY;
  for (size_t index = 0; index < logRingCount; ++index) {
    const LogEntry &entry = logRing[(firstEntry + index) % LOG_CAPACITY];
    Serial.printf("  [%lu ms] [%s] %s\n",
                  static_cast<unsigned long>(entry.uptimeMs),
                  logLevelName(entry.level), entry.message);
  }
  xSemaphoreGive(logMutex);
}

bool setLogLevel(const char *levelName) {
  if (strcmp(levelName, "error") == 0) currentLogLevel = LogLevel::Error;
  else if (strcmp(levelName, "warn") == 0) currentLogLevel = LogLevel::Warn;
  else if (strcmp(levelName, "info") == 0) currentLogLevel = LogLevel::Info;
  else if (strcmp(levelName, "debug") == 0) currentLogLevel = LogLevel::Debug;
  else return false;
  return true;
}

void printPreviousWatchdogIncident() {
  if (lastWatchdogIncident.magic != WATCHDOG_INCIDENT_MAGIC) {
    return;
  }

  Serial.println(F("Previous watchdog incident:"));
  Serial.printf("  reason=%s\n", lastWatchdogIncident.reason);
  Serial.printf("  event_uptime_ms=%lu\n",
                static_cast<unsigned long>(lastWatchdogIncident.eventUptimeMs));
  Serial.printf("  last_cursor_heartbeat_ms=%lu\n",
                static_cast<unsigned long>(lastWatchdogIncident.lastCursorHeartbeatMs));
  Serial.printf("  free_heap=%lu, minimum_free_heap=%lu\n",
                static_cast<unsigned long>(lastWatchdogIncident.freeHeap),
                static_cast<unsigned long>(lastWatchdogIncident.minimumFreeHeap));
  Serial.printf("  reset_reason_before_restart=%lu, watchdog_core=%lu\n",
                static_cast<unsigned long>(lastWatchdogIncident.resetReason),
                static_cast<unsigned long>(lastWatchdogIncident.watchdogCore));

  lastWatchdogIncident.magic = 0;
}

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
      logMessage(LogLevel::Info, "Button command applied: %s, interval=%lu ms",
             command.text, static_cast<unsigned long>(result.intervalMs));
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
  uint32_t nextCursorToggleMs = millis() + blinkIntervalMs;

  for (;;) {
    uint32_t requestedIntervalMs;
    if (xQueueReceive(blinkIntervalQueue, &requestedIntervalMs, 0) == pdTRUE) {
      blinkIntervalMs = requestedIntervalMs;
      nextCursorToggleMs = millis() + blinkIntervalMs;
    }

    uint32_t nowMs = millis();
    if (static_cast<int32_t>(nowMs - nextCursorToggleMs) < 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
      cursorHeartbeatMs = millis();
      continue;
    }

    cursorVisible = !cursorVisible;
    xSemaphoreTake(displayMutex, portMAX_DELAY);
    display.fillRect(122, 56, 6, 8,
                     cursorVisible ? SSD1306_WHITE : SSD1306_BLACK);
    display.display();
    xSemaphoreGive(displayMutex);
    nextCursorToggleMs = nowMs + blinkIntervalMs;
    cursorHeartbeatMs = millis();
  }
}

void radioTask(void *parameter) {
  (void)parameter;
  pinMode(LORA_DIO0, INPUT);

  int16_t status = radio.begin(LORA_FREQUENCY, 125.0, 11, 5, 0x12, 10, 8);
  if (status != RADIOLIB_ERR_NONE) {
    logMessage(LogLevel::Error, "Radio initialization failed, code=%d", status);
    vTaskDelete(nullptr);
  }
  logMessage(LogLevel::Info, "RadioTask ready: LoRa SF11, packet=%u bytes",
             static_cast<unsigned>(LORA_PACKET_SIZE));

  uint8_t packet[LORA_PACKET_SIZE];
  for (size_t index = 0; index < sizeof(packet); ++index) {
    packet[index] = static_cast<uint8_t>(index);
  }

  bool request;
  for (;;) {
    if (xQueueReceive(radioTransmitQueue, &request, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    uint32_t transmissionStartedMs = millis();
    status = radio.startTransmit(packet, sizeof(packet));
    if (status != RADIOLIB_ERR_NONE) {
      logMessage(LogLevel::Error, "Radio startTransmit failed, code=%d", status);
      continue;
    }

    while (digitalRead(LORA_DIO0) == LOW) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }

    status = radio.finishTransmit();
    uint32_t transmissionTimeMs = millis() - transmissionStartedMs;
    logMessage(status == RADIOLIB_ERR_NONE ? LogLevel::Info : LogLevel::Error,
           "LoRa TX: %u bytes, elapsed=%lu ms, result=%d",
           static_cast<unsigned>(sizeof(packet)),
           static_cast<unsigned long>(transmissionTimeMs), status);

    uint32_t quietTimeMs = transmissionTimeMs *
                           (100 - LORA_DUTY_CYCLE_PERCENT) /
                           LORA_DUTY_CYCLE_PERCENT;
    if (quietTimeMs > 0) {
      logMessage(LogLevel::Debug, "Duty cycle %u%%, waiting %lu ms",
             static_cast<unsigned>(LORA_DUTY_CYCLE_PERCENT),
             static_cast<unsigned long>(quietTimeMs));
      vTaskDelay(pdMS_TO_TICKS(quietTimeMs));
    }
  }
}

void watchdogTask(void *parameter) {
  (void)parameter;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    uint32_t nowMs = millis();
    uint32_t lastHeartbeatMs = cursorHeartbeatMs;

    if (nowMs - lastHeartbeatMs <= WATCHDOG_TIMEOUT_MS) {
      continue;
    }

    lastWatchdogIncident.magic = WATCHDOG_INCIDENT_MAGIC;
    lastWatchdogIncident.eventUptimeMs = nowMs;
    lastWatchdogIncident.lastCursorHeartbeatMs = lastHeartbeatMs;
    lastWatchdogIncident.freeHeap = ESP.getFreeHeap();
    lastWatchdogIncident.minimumFreeHeap = ESP.getMinFreeHeap();
    lastWatchdogIncident.resetReason = static_cast<uint32_t>(esp_reset_reason());
    lastWatchdogIncident.watchdogCore = xPortGetCoreID();
    strncpy(lastWatchdogIncident.reason,
            "CursorTask heartbeat timeout; possible mutex deadlock",
            sizeof(lastWatchdogIncident.reason) - 1);

    logMessage(LogLevel::Error,
           "WATCHDOG: CursorTask timeout, reboot in 100 ms, event=%lu ms, heartbeat=%lu ms",
           static_cast<unsigned long>(nowMs),
           static_cast<unsigned long>(lastHeartbeatMs));
    Serial.flush();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
  }
}

void deadlockTask(void *parameter) {
  (void)parameter;
  bool deadlockRequested;

  for (;;) {
    if (xQueueReceive(deadlockQueue, &deadlockRequested, portMAX_DELAY) == pdTRUE) {
      xSemaphoreTake(displayMutex, portMAX_DELAY);
      logMessage(LogLevel::Warn,
             "DeadlockTask owns displayMutex and will never release it");
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
          logMessage(LogLevel::Warn, "Deadlock requested");
        }
      } else if (strcmp(command, "version") == 0) {
        printVersion();
        queued = true;
      } else if (strncmp(command, "loglevel ", 9) == 0) {
        queued = setLogLevel(command + 9);
        if (queued) {
          logMessage(LogLevel::Info, "Log level changed to %s", command + 9);
        } else {
          Serial.println(F("Usage: loglevel error|warn|info|debug"));
        }
      } else if (strcmp(command, "logserial on") == 0 ||
                 strcmp(command, "logserial off") == 0) {
        serialLoggingEnabled = strcmp(command, "logserial on") == 0;
        queued = true;
        Serial.printf("Serial logging: %s\n",
                      serialLoggingEnabled ? "on" : "off");
      } else if (strcmp(command, RADIO_COMMAND) == 0) {
        bool trigger = true;
        queued = xQueueSend(radioTransmitQueue, &trigger, 0) == pdTRUE;
        if (queued) {
          logMessage(LogLevel::Info, "Radio transmission queued: 32 bytes");
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
        logMessage(LogLevel::Warn, "Unknown command or full queue: %s", command);
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
  delay(100);
  printPreviousWatchdogIncident();
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.display();

  buttonEventQueue = xQueueCreate(8, sizeof(SerialButtonCommand));
  blinkIntervalQueue = xQueueCreate(2, sizeof(uint32_t));
  serialResultQueue = xQueueCreate(8, sizeof(SerialCommandResult));
  deadlockQueue = xQueueCreate(1, sizeof(bool));
  radioTransmitQueue = xQueueCreate(2, sizeof(bool));
  displayMutex = xSemaphoreCreateMutex();
  logMutex = xSemaphoreCreateMutex();
  if (buttonEventQueue == nullptr || blinkIntervalQueue == nullptr ||
      serialResultQueue == nullptr || deadlockQueue == nullptr ||
      radioTransmitQueue == nullptr || displayMutex == nullptr || logMutex == nullptr) {
    while (true) {
      delay(1000);
    }
  }

  xTaskCreatePinnedToCore(buttonTask, "ButtonTask", 2048, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(cursorTask, "CursorTask", 2048, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(deadlockTask, "DeadlockTask", 2048, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(radioTask, "RadioTask", 4096, nullptr, 1, nullptr, 0);
  cursorHeartbeatMs = millis();
  xTaskCreatePinnedToCore(watchdogTask, "WatchdogTask", 3072, nullptr, 2, nullptr, 1);
  logMessage(LogLevel::Info, "System started, build=%s", BUILD_HASH);
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