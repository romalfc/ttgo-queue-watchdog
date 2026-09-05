#include "config_store.h"

#include <Preferences.h>

namespace {

struct SchemaDescriptor {
  uint16_t version;
  const char *logLevelKey;
  const char *serialLoggingKey;
  const char *watchdogTimeoutKey;
  const char *dutyCycleKey;
};

constexpr SchemaDescriptor SCHEMAS[] = {
    {ConfigSchema::VersionV01, nullptr,
     ConfigSchema::LegacySerialLoggingKey,
     ConfigSchema::LegacyWatchdogTimeoutKey, nullptr},
    {ConfigSchema::VersionV02, ConfigSchema::LogLevelKey,
     ConfigSchema::SerialLoggingKey, ConfigSchema::WatchdogTimeoutKey,
     ConfigSchema::DutyCycleKey},
};

const SchemaDescriptor *findSchema(uint16_t version) {
  for (const SchemaDescriptor &schema : SCHEMAS) {
    if (schema.version == version) {
      return &schema;
    }
  }
  return nullptr;
}

AppConfig defaultConfig() {
  return {CONFIG_VERSION, LOGGER_DEFAULT_LEVEL,
          LOGGER_SERIAL_ENABLED != 0, DEFAULT_WATCHDOG_TIMEOUT_MS,
          DEFAULT_DUTY_CYCLE_PERCENT};
}

bool isValidConfig(const AppConfig &config) {
  return config.logLevel <= LogLevel::Debug &&
         config.watchdogTimeoutMs >= MIN_WATCHDOG_TIMEOUT_MS &&
         config.watchdogTimeoutMs <= MAX_WATCHDOG_TIMEOUT_MS &&
         config.dutyCyclePercent >= 1 && config.dutyCyclePercent <= 100;
}

}

bool ConfigStore::writeSchema(uint16_t version, const AppConfig &config) {
  const SchemaDescriptor *schema = findSchema(version);
  if (schema == nullptr) {
    return false;
  }

  Preferences preferences;
  preferences.begin("app-config", false);
  preferences.putUShort("cfg_version", schema->version);
  if (schema->logLevelKey != nullptr) {
    preferences.putUChar(schema->logLevelKey,
                         static_cast<uint8_t>(config.logLevel));
  }
  preferences.putBool(schema->serialLoggingKey, config.serialLogging);
  preferences.putUInt(schema->watchdogTimeoutKey, config.watchdogTimeoutMs);
  if (schema->dutyCycleKey != nullptr) {
    preferences.putUChar(schema->dutyCycleKey, config.dutyCyclePercent);
  }
  preferences.end();
  return true;
}

AppConfig ConfigStore::load() {
  AppConfig config = defaultConfig();
  Preferences preferences;
  preferences.begin("app-config", false);
  uint16_t storedVersion = preferences.getUShort("cfg_version", 0);
  const SchemaDescriptor *schema = findSchema(storedVersion);

  if (schema == nullptr) {
    preferences.end();
    writeSchema(ConfigSchema::CurrentVersion, config);
    return config;
  }

  if (schema->logLevelKey != nullptr) {
    config.logLevel = static_cast<LogLevel>(preferences.getUChar(
        schema->logLevelKey, static_cast<uint8_t>(config.logLevel)));
  }
  config.serialLogging = preferences.getBool(schema->serialLoggingKey,
                                              config.serialLogging);
  config.watchdogTimeoutMs = preferences.getUInt(schema->watchdogTimeoutKey,
                                                 config.watchdogTimeoutMs);
  if (schema->dutyCycleKey != nullptr) {
    config.dutyCyclePercent = preferences.getUChar(schema->dutyCycleKey,
                                                   config.dutyCyclePercent);
  }
  preferences.end();

  bool validConfig = isValidConfig(config);
  if (!validConfig) {
    config = defaultConfig();
  }
  config.version = ConfigSchema::CurrentVersion;
  if (storedVersion != ConfigSchema::CurrentVersion || !validConfig) {
    writeSchema(ConfigSchema::CurrentVersion, config);
  }
  return config;
}

void ConfigStore::save(AppConfig &config) {
  config.version = ConfigSchema::CurrentVersion;
  writeSchema(ConfigSchema::CurrentVersion, config);
}

void ConfigStore::reset(AppConfig &config) {
  config = defaultConfig();
  writeSchema(ConfigSchema::CurrentVersion, config);
}

bool ConfigStore::migrate(uint16_t targetVersion, AppConfig &config) {
  if (findSchema(targetVersion) == nullptr) {
    return false;
  }

  AppConfig current = load();
  if (!writeSchema(targetVersion, current)) {
    return false;
  }
  current.version = targetVersion;
  config = current;
  return true;
}
