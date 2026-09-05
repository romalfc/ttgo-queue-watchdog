#include "services/logger.h"

#include <stdarg.h>

Logger::Logger() : mutex_(nullptr) {}

void Logger::ensureMutex() {
  if (mutex_ == nullptr) {
    mutex_ = xSemaphoreCreateMutex();
  }
}

const char *Logger::levelName(LogLevel level) {
  switch (level) {
    case LogLevel::Error: return "ERROR";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Info: return "INFO";
    case LogLevel::Debug: return "DEBUG";
  }
  return "UNKNOWN";
}

void Logger::message(LogLevel level, const char *format, ...) {
  ensureMutex();
  LogEntry entry{};
  entry.uptimeMs = millis();
  entry.level = level;
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(entry.message, sizeof(entry.message), format, arguments);
  va_end(arguments);

  xSemaphoreTake(mutex_, portMAX_DELAY);
  ring_[next_] = entry;
  next_ = (next_ + 1) % LOG_CAPACITY;
  if (count_ < LOG_CAPACITY) ++count_;
  if (serialEnabled_ && level <= currentLevel_) {
    Serial.printf("[%lu ms] [%s] %s\n",
                  static_cast<unsigned long>(entry.uptimeMs),
                  levelName(level), entry.message);
  }
  xSemaphoreGive(mutex_);
}

void Logger::printVersion(const char *buildHash, uint8_t dutyCyclePercent) {
  ensureMutex();
  xSemaphoreTake(mutex_, portMAX_DELAY);
  Serial.println(F("Build information:"));
  Serial.printf("  hash=%s\n", buildHash);
  Serial.println(F("  platform=ESP32 TTGO LoRa32"));
  Serial.println(F("  framework=Arduino/FreeRTOS"));
  Serial.println(F("  radio=LoRa 868 MHz, SF11, BW125 kHz, CR5, 32 bytes"));
  Serial.printf("  duty_cycle=%u%%\n", static_cast<unsigned>(dutyCyclePercent));
  Serial.printf("  log_level=%s\n", levelName(currentLevel_));
  Serial.printf("  serial_logging=%s\n", serialEnabled_ ? "on" : "off");
  Serial.printf("  ring_entries=%u/%u\n", static_cast<unsigned>(count_),
                static_cast<unsigned>(LOG_CAPACITY));
  Serial.println(F("Ring log:"));
  size_t first = (next_ + LOG_CAPACITY - count_) % LOG_CAPACITY;
  for (size_t index = 0; index < count_; ++index) {
    const LogEntry &entry = ring_[(first + index) % LOG_CAPACITY];
    Serial.printf("  [%lu ms] [%s] %s\n",
                  static_cast<unsigned long>(entry.uptimeMs),
                  levelName(entry.level), entry.message);
  }
  xSemaphoreGive(mutex_);
}

bool Logger::setLevel(const char *levelNameValue) {
  if (strcmp(levelNameValue, "error") == 0) currentLevel_ = LogLevel::Error;
  else if (strcmp(levelNameValue, "warn") == 0) currentLevel_ = LogLevel::Warn;
  else if (strcmp(levelNameValue, "info") == 0) currentLevel_ = LogLevel::Info;
  else if (strcmp(levelNameValue, "debug") == 0) currentLevel_ = LogLevel::Debug;
  else return false;
  return true;
}

void Logger::setLevel(LogLevel levelValue) { currentLevel_ = levelValue; }
LogLevel Logger::level() const { return currentLevel_; }
void Logger::setSerialEnabled(bool enabled) { serialEnabled_ = enabled; }
bool Logger::serialEnabled() const { return serialEnabled_; }
