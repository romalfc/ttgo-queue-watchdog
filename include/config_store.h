#pragma once

#include "config.h"

class ConfigStore {
 public:
  AppConfig load();
  void save(AppConfig &config);
  void reset(AppConfig &config);
  bool migrate(uint16_t targetVersion, AppConfig &config);

 private:
  bool writeSchema(uint16_t version, const AppConfig &config);
};
