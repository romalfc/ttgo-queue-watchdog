#pragma once

#include <Arduino.h>

#include "config.h"

struct LogEntry {
  uint32_t uptimeMs;
  LogLevel level;
  char message[LOG_MESSAGE_SIZE];
};

class Logger {
 public:
  Logger();
  void message(LogLevel level, const char *format, ...);
  void printVersion(const char *buildHash, uint8_t dutyCyclePercent);
  bool setLevel(const char *levelName);
  void setLevel(LogLevel level);
  LogLevel level() const;
  void setSerialEnabled(bool enabled);
  bool serialEnabled() const;
  static const char *levelName(LogLevel level);

 private:
  void ensureMutex();
  SemaphoreHandle_t mutex_;
  LogEntry ring_[LOG_CAPACITY]{};
  size_t next_ = 0;
  size_t count_ = 0;
  LogLevel currentLevel_ = LOGGER_DEFAULT_LEVEL;
  bool serialEnabled_ = LOGGER_SERIAL_ENABLED != 0;
};
