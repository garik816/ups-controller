#pragma once

#include <ESP8266WebServer.h>
#include <Ticker.h>
#include <UpsController.h>
#include <BatteryMonitor.h>
#include "NetworkManager.h"
#include "Settings.h"

class App {
public:
  void setup();
  void loop();
private:
  struct LogEntry {
    uint32_t id;
    uint32_t uptime_ms;
    char level[9];
    char message[160];
  };
  struct RtcRecord {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t attempts;
    uint32_t checksum;
  };
  static constexpr uint8_t LogCapacity = 24;
  Settings settings_;
  SettingsStore store_;
  NetworkManager network_;
  ups::Controller controller_;
  ups::BatteryMonitor battery_monitor_;
  ESP8266WebServer server_{80};
  Ticker relay_timer_;
  LogEntry logs_[LogCapacity] = {};
  uint8_t log_count_ = 0;
  uint8_t log_head_ = 0;
  uint32_t next_log_id_ = 1;
  char csrf_token_[33] = {};
  RtcRecord rtc_cache_ = {};
  bool rtc_ok_ = true;
  bool requested_relay_ = false;
  volatile bool relay_cutoff_ = false;
  bool network_pending_ = false;
  uint32_t network_apply_at_ = 0;
  bool reboot_pending_ = false;
  uint32_t reboot_requested_at_ = 0;
  bool uploading_ = false;
  bool upload_started_ = false;
  bool upload_done_ = false;
  String upload_error_;
  int upload_error_code_ = 400;
  char serial_line_[32] = {};
  uint8_t serial_length_ = 0;
  bool serial_overflow_ = false;
  bool battery_low_ = false;
  bool battery_overrange_ = false;

  void routes();
  void serialCommands();
  void updateController();
  void updateBattery();
  void setRelayFromController();
  void log(const char *level, const char *message);
  static void coreEvent(void *context, ups::Event event);
  static void networkEvent(void *context, const char *level, const char *message);
  bool authenticate(bool mutation = false);
  bool tokenMatches() const;
  bool idleForMaintenance();
  bool jsonBody(DynamicJsonDocument &document);
  void sendJson(JsonDocument &document, int status = 200);
  void success(const char *message);
  void error(int status, const char *message);
  void sendStatus();
  void sendConfig();
  void saveConfig();
  void calibrateBattery();
  void sendLogs();
  void action();
  void uploadChunk();
  void uploadFinished();
  bool restoreRtc(ups::Latch &latch);
  void persistRtc();
  static uint32_t rtcChecksum(const RtcRecord &record);
};
