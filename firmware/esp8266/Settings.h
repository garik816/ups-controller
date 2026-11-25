#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <UpsController.h>
#include <BatteryMonitor.h>

enum class NetworkMode : uint8_t { Standalone, Router, Both };

struct Settings {
  NetworkMode network_mode = NetworkMode::Standalone;
  char ssid[33] = {};
  char wifi_password[64] = {};
  char admin_password[64] = "1234567890";
  uint16_t fallback_seconds = 30;
  ups::Config control;
  ups::BatteryConfig battery;
  bool online_active_high = true;
  bool battery_active_high = true;
  bool standby_active_high = true;
};

const char *networkModeName(NetworkMode mode);
void settingsToJson(const Settings &settings, JsonObject root, bool includeSecrets);
bool patchSettings(JsonObjectConst root, Settings &settings, String &error);

class SettingsStore {
public:
  bool begin(Settings &settings, String &message);
  bool save(const Settings &settings, String &error);
  bool healthy() const { return healthy_; }
private:
  bool mounted_ = false;
  bool healthy_ = false;
  bool readFile(const char *path, Settings &settings);
};
