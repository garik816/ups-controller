#pragma once

#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include "Settings.h"

class NetworkManager {
public:
  typedef void (*Logger)(void *, const char *, const char *);
  void setLogger(Logger logger, void *context) { logger_ = logger; context_ = context; }
  void begin(const Settings &settings, uint32_t now);
  void tick(uint32_t now);
  void status(JsonObject object) const;
  bool startScan();
  bool scanning() const;
  bool hasConflict() const { return conflict_; }
  void scanResults(JsonObject object);
private:
  Settings settings_;
  Logger logger_ = nullptr;
  void *context_ = nullptr;
  bool ap_active_ = false;
  bool connected_ = false;
  bool fallback_ = false;
  bool conflict_ = false;
  bool scan_requested_ = false;
  bool scan_station_enabled_ = false;
  uint32_t disconnected_at_ = 0;
  uint32_t connected_at_ = 0;
  uint32_t last_reconnect_at_ = 0;
  uint32_t last_ap_attempt_at_ = 0;
  void setAp(bool enabled);
  void log(const char *level, const char *message);
  bool overlappingSubnet() const;
};
