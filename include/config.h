#pragma once

#include <Arduino.h>

#ifndef LOGGER_SERIAL_ENABLED
#define LOGGER_SERIAL_ENABLED 1
#endif

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_RESET (-1)
#define BUTTON_PIN 0
namespace ConfigSchema {

constexpr uint16_t VersionV01 = 1;
constexpr uint16_t VersionV02 = 2;
constexpr uint16_t CurrentVersion = VersionV02;

constexpr char LegacySerialLoggingKey[] = "serial_log";
constexpr char LegacyWatchdogTimeoutKey[] = "watchdog_ms";
constexpr char LogLevelKey[] = "log_level";
constexpr char SerialLoggingKey[] = "serial_logging";
constexpr char WatchdogTimeoutKey[] = "wd_timeout_ms";
constexpr char DutyCycleKey[] = "duty_pct";

static_assert(sizeof(LegacySerialLoggingKey) - 1 <= 15,
              "NVS key exceeds the 15-character limit");
static_assert(sizeof(LegacyWatchdogTimeoutKey) - 1 <= 15,
              "NVS key exceeds the 15-character limit");
static_assert(sizeof(LogLevelKey) - 1 <= 15,
              "NVS key exceeds the 15-character limit");
static_assert(sizeof(SerialLoggingKey) - 1 <= 15,
              "NVS key exceeds the 15-character limit");
static_assert(sizeof(WatchdogTimeoutKey) - 1 <= 15,
              "NVS key exceeds the 15-character limit");
static_assert(sizeof(DutyCycleKey) - 1 <= 15,
              "NVS key exceeds the 15-character limit");

constexpr uint32_t DefaultWatchdogTimeoutMs = 5000;
constexpr uint8_t DefaultDutyCyclePercent = 1;
constexpr uint32_t MinWatchdogTimeoutMs = 1000;
constexpr uint32_t MaxWatchdogTimeoutMs = 60000;

}

constexpr uint16_t CONFIG_VERSION = ConfigSchema::CurrentVersion;
constexpr uint32_t DEFAULT_WATCHDOG_TIMEOUT_MS = ConfigSchema::DefaultWatchdogTimeoutMs;
constexpr uint8_t DEFAULT_DUTY_CYCLE_PERCENT = ConfigSchema::DefaultDutyCyclePercent;
constexpr uint32_t MIN_WATCHDOG_TIMEOUT_MS = ConfigSchema::MinWatchdogTimeoutMs;
constexpr uint32_t MAX_WATCHDOG_TIMEOUT_MS = ConfigSchema::MaxWatchdogTimeoutMs;
#define LORA_CS 18
#define LORA_DIO0 26
#define LORA_DIO1 33
#define LORA_RST 23
#define LORA_FREQUENCY 868.0

constexpr char SINGLE_BUTTON_COMMAND[] = "single-btn";
constexpr char DOUBLE_BUTTON_COMMAND[] = "double-btn";
constexpr char DEADLOCK_COMMAND[] = "deadlock";
constexpr char RADIO_COMMAND[] = "radio";

constexpr uint32_t DOUBLE_CLICK_THRESHOLD_MS = 350;
constexpr uint32_t WATCHDOG_INCIDENT_MAGIC = 0x57444331;
constexpr size_t LORA_PACKET_SIZE = 32;
constexpr size_t LOG_CAPACITY = 32;
constexpr size_t LOG_MESSAGE_SIZE = 96;

constexpr uint32_t BLINK_INTERVALS_MS[] = {100, 500, 1000, 2000};
constexpr size_t BLINK_INTERVAL_COUNT = sizeof(BLINK_INTERVALS_MS) /
                                         sizeof(BLINK_INTERVALS_MS[0]);
constexpr TickType_t BUTTON_DEBOUNCE = pdMS_TO_TICKS(40);
constexpr TickType_t DOUBLE_CLICK_WINDOW = pdMS_TO_TICKS(350);

enum class LogLevel : uint8_t {
  Error = 0,
  Warn,
  Info,
  Debug
};

constexpr LogLevel LOGGER_DEFAULT_LEVEL = LogLevel::Info;

struct AppConfig {
  uint16_t version;
  LogLevel logLevel;
  bool serialLogging;
  uint32_t watchdogTimeoutMs;
  uint8_t dutyCyclePercent;
};
