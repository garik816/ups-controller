#include <cstdlib>
#include <iostream>
#include <string>

#include "Settings.h"
#include <LittleFS.h>

MemoryLittleFS LittleFS;

#define CHECK(expression) do { \
  if (!(expression)) { \
    std::cerr << __func__ << ':' << __LINE__ << " failed: " #expression << '\n'; \
    std::exit(1); \
  } \
} while (false)

bool apply(const char *json, Settings &settings) {
  DynamicJsonDocument document(4096);
  CHECK(!deserializeJson(document, json));
  String error;
  return patchSettings(document.as<JsonObjectConst>(), settings, error);
}

Settings customized() {
  Settings settings;
  CHECK(apply(R"({"network":{"mode":"both","ssid":"UPS test router","password":"router-secret-834","fallback_seconds":45},"security":{"password":"admin-secret-627"},"ups":{"auto_mode":"keep_on","max_attempts":3,"start_delay_ms":22000,"auto_on_boot":false},"inputs":{"battery_active_high":false}})", settings));
  return settings;
}

std::string snapshotJson(const Settings &settings) {
  DynamicJsonDocument document(4096);
  settingsToJson(settings, document.to<JsonObject>(), true);
  document["schema"] = 1;
  std::string output;
  serializeJson(document, output);
  return output;
}

void strict_json_types_and_numeric_bounds() {
  const char *invalid[] = {
    R"({"ups":{"start_delay_ms":"1000"}})",
    R"({"ups":{"start_delay_ms":true}})",
    R"({"ups":{"start_delay_ms":1000.5}})",
    R"({"ups":{"start_delay_ms":-1}})",
    R"({"ups":{"start_delay_ms":4294967296}})",
    R"({"ups":{"start_delay_ms":999}})",
    R"({"ups":{"start_delay_ms":600001}})",
    R"({"ups":{"press_ms":99}})",
    R"({"ups":{"press_ms":10001}})",
    R"({"ups":{"max_attempts":0}})",
    R"({"ups":{"max_attempts":4}})",
    R"({"ups":{"auto_on_boot":1}})",
    R"({"ups":{"allow_battery_start":"false"}})",
    R"({"inputs":{"online_active_high":null}})",
    R"({"inputs":{"online_active_high":0}})",
    R"({"network":[]})",
    R"({"network":null})",
    R"({"security":{"password":1234567890}})",
    R"({"ups":{"auto_mode":true}})",
    R"({"ups":{"auto_mode":"retry_forever"}})",
    R"({"network":{"mode":"internet"}})",
    R"({"network":{"fallback_seconds":121}})"
  };
  for (const char *json : invalid) {
    Settings candidate;
    CHECK(!apply(json, candidate));
  }
  Settings valid;
  CHECK(apply(R"({"ups":{"start_delay_ms":600000,"press_ms":100,"stop_press_ms":10000,"verify_timeout_ms":120000,"indicator_filter_ms":5000,"off_confirm_ms":30000,"max_attempts":3,"auto_on_boot":false},"network":{"fallback_seconds":120}})", valid));
  CHECK(valid.control.start_delay_ms == 600000);
  CHECK(!valid.control.auto_on_boot);
}

void related_timing_constraints_reject_impossible_confirmation() {
  Settings config;
  CHECK(!apply(R"({"ups":{"indicator_filter_ms":1000,"off_confirm_ms":500}})", config));
  config = Settings();
  CHECK(!apply(R"({"ups":{"verify_timeout_ms":2000,"off_confirm_ms":2000}})", config));
  config = Settings();
  CHECK(apply(R"({"ups":{"indicator_filter_ms":500,"off_confirm_ms":1000,"verify_timeout_ms":1001}})", config));
}

void omitted_passwords_are_preserved_and_empty_wifi_password_clears() {
  Settings config = customized();
  CHECK(apply(R"({"network":{"ssid":"another router"},"ups":{"start_delay_ms":15000}})", config));
  CHECK(std::string(config.wifi_password) == "router-secret-834");
  CHECK(std::string(config.admin_password) == "admin-secret-627");
  CHECK(apply(R"({"network":{"password":""}})", config));
  CHECK(config.wifi_password[0] == 0);
  CHECK(!apply(R"({"security":{"password":""}})", config));
}

void strings_use_byte_limits_and_reject_embedded_nul() {
  Settings config;
  CHECK(!apply(R"({"network":{"mode":"router"}})", config));
  config = Settings();
  CHECK(!apply(R"({"network":{"ssid":"ok\u0000hidden"}})", config));
  CHECK(!apply(R"({"network":{"password":"abcdefgh\u0000hidden"}})", config));
  CHECK(!apply(R"({"security":{"password":"abcdefgh\u0000hidden"}})", config));
  CHECK(!apply(R"({"network":{"password":"1234567"}})", config));
  const std::string too_long_password(64, 'x');
  const std::string password_json = "{\"network\":{\"password\":\"" + too_long_password + "\"}}";
  CHECK(!apply(password_json.c_str(), config));
  std::string unicode_name;
  for (unsigned i = 0; i < 16; ++i) unicode_name += "\xD0\x90";
  const std::string valid_json = "{\"network\":{\"mode\":\"router\",\"ssid\":\"" + unicode_name + "\"}}";
  config = Settings();
  CHECK(apply(valid_json.c_str(), config));
  unicode_name += "\xD0\x90";
  const std::string invalid_json = "{\"network\":{\"ssid\":\"" + unicode_name + "\"}}";
  CHECK(!apply(invalid_json.c_str(), config));
}

void public_configuration_never_contains_secrets() {
  const Settings config = customized();
  DynamicJsonDocument document(4096);
  settingsToJson(config, document.to<JsonObject>(), false);
  CHECK(!document["network"].containsKey("password"));
  CHECK(!document["security"].containsKey("password"));
  CHECK(document["network"]["has_password"].as<bool>());
  std::string serialized;
  serializeJson(document, serialized);
  CHECK(serialized.find(config.wifi_password) == std::string::npos);
  CHECK(serialized.find(config.admin_password) == std::string::npos);
  CHECK(serialized.find(config.ssid) != std::string::npos);
}

void valid_snapshot_round_trips_through_actual_store() {
  LittleFS.reset();
  Settings initial;
  SettingsStore store;
  String message;
  CHECK(store.begin(initial, message));
  CHECK(store.healthy());
  const Settings expected = customized();
  CHECK(store.save(expected, message));
  CHECK(LittleFS.contents("/config.json").find(expected.wifi_password) != std::string::npos);
  Settings actual;
  SettingsStore reopened;
  CHECK(reopened.begin(actual, message));
  CHECK(actual.network_mode == NetworkMode::Both);
  CHECK(std::string(actual.ssid) == expected.ssid);
  CHECK(std::string(actual.wifi_password) == expected.wifi_password);
  CHECK(std::string(actual.admin_password) == expected.admin_password);
  CHECK(actual.control.auto_mode == ups::AutoMode::KeepOn);
  CHECK(actual.control.start_delay_ms == expected.control.start_delay_ms);
  CHECK(!actual.control.auto_on_boot);
  CHECK(!actual.battery_active_high);
}

void corrupt_primary_uses_previous_complete_backup() {
  LittleFS.reset();
  Settings old = customized();
  old.control.start_delay_ms = 11000;
  SettingsStore store;
  Settings boot;
  String message;
  CHECK(store.begin(boot, message));
  CHECK(store.save(old, message));
  Settings newer = old;
  newer.control.start_delay_ms = 33000;
  CHECK(store.save(newer, message));
  LittleFS.seed("/config.json", "{truncated");
  Settings restored;
  SettingsStore reopened;
  CHECK(reopened.begin(restored, message));
  CHECK(reopened.healthy());
  CHECK(restored.control.start_delay_ms == 11000);
  CHECK(std::string(restored.wifi_password) == old.wifi_password);
}

void incomplete_or_invalid_snapshot_without_backup_disables_autostart() {
  const char *invalid_files[] = {"{", "{\"schema\":1}", "[]", "null"};
  for (const char *file : invalid_files) {
    LittleFS.reset();
    LittleFS.seed("/config.json", file);
    Settings config;
    SettingsStore store;
    String message;
    CHECK(!store.begin(config, message));
    CHECK(!store.healthy());
    CHECK(config.control.auto_mode == ups::AutoMode::Disabled);
  }
  LittleFS.reset();
  DynamicJsonDocument document(4096);
  CHECK(!deserializeJson(document, snapshotJson(customized())));
  document["ups"]["press_ms"] = "2000";
  std::string invalid;
  serializeJson(document, invalid);
  LittleFS.seed("/config.json", invalid);
  Settings config;
  SettingsStore store;
  String message;
  CHECK(!store.begin(config, message));
  CHECK(config.control.auto_mode == ups::AutoMode::Disabled);
}

void incomplete_primary_falls_back_to_valid_backup() {
  LittleFS.reset();
  Settings expected = customized();
  LittleFS.seed("/config.json", "{\"schema\":1}");
  LittleFS.seed("/config.bak", snapshotJson(expected));
  Settings actual;
  SettingsStore store;
  String message;
  CHECK(store.begin(actual, message));
  CHECK(actual.control.auto_mode == expected.control.auto_mode);
  CHECK(std::string(actual.admin_password) == expected.admin_password);
}

void mount_failure_disables_control_and_refuses_save() {
  LittleFS.reset();
  LittleFS.mount_ok = false;
  Settings config;
  SettingsStore store;
  String message;
  CHECK(!store.begin(config, message));
  CHECK(config.control.auto_mode == ups::AutoMode::Disabled);
  CHECK(!store.save(customized(), message));
}

void short_write_does_not_replace_previous_configuration() {
  LittleFS.reset();
  Settings old = customized();
  SettingsStore store;
  Settings boot;
  String message;
  CHECK(store.begin(boot, message));
  CHECK(store.save(old, message));
  const std::string original = LittleFS.contents("/config.json");
  Settings next = old;
  next.control.start_delay_ms = 99000;
  LittleFS.write_limit = 16;
  CHECK(!store.save(next, message));
  CHECK(LittleFS.contents("/config.json") == original);
  CHECK(!LittleFS.exists("/config.tmp"));
  Settings restored;
  SettingsStore reopened;
  CHECK(reopened.begin(restored, message));
  CHECK(restored.control.start_delay_ms == old.control.start_delay_ms);
}

void interrupted_final_rename_recovers_previous_backup() {
  LittleFS.reset();
  Settings old = customized();
  SettingsStore store;
  Settings boot;
  String message;
  CHECK(store.begin(boot, message));
  CHECK(store.save(old, message));
  Settings next = old;
  next.control.start_delay_ms = 99000;
  LittleFS.fail_rename_number = 2;
  CHECK(!store.save(next, message));
  CHECK(!LittleFS.exists("/config.json"));
  CHECK(LittleFS.exists("/config.bak"));
  Settings restored;
  SettingsStore reopened;
  CHECK(reopened.begin(restored, message));
  CHECK(restored.control.start_delay_ms == old.control.start_delay_ms);
  CHECK(std::string(restored.admin_password) == old.admin_password);
}

int main() {
  strict_json_types_and_numeric_bounds();
  related_timing_constraints_reject_impossible_confirmation();
  omitted_passwords_are_preserved_and_empty_wifi_password_clears();
  strings_use_byte_limits_and_reject_embedded_nul();
  public_configuration_never_contains_secrets();
  valid_snapshot_round_trips_through_actual_store();
  corrupt_primary_uses_previous_complete_backup();
  incomplete_or_invalid_snapshot_without_backup_disables_autostart();
  incomplete_primary_falls_back_to_valid_backup();
  mount_failure_disables_control_and_refuses_save();
  short_write_does_not_replace_previous_configuration();
  interrupted_final_rename_recovers_previous_backup();
  std::cout << "12 settings validation/persistence tests passed\n";
  return 0;
}
