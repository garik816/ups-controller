#include "Settings.h"

#include <LittleFS.h>
#include "BoardConfig.h"

namespace {
constexpr size_t MaxConfigBytes = 4096;

bool fail(String &error, const char *field) {
  error = F("Неприпустиме значення: ");
  error += field;
  return false;
}

bool copyText(JsonObjectConst root, const char *key, char *dest, size_t capacity,
              String &error) {
  if (!root.containsKey(key)) return true;
  if (!root[key].is<const char *>()) return fail(error, key);
  JsonString text = root[key].as<JsonString>();
  if (text.size() >= capacity || strlen(text.c_str()) != text.size()) return fail(error, key);
  memcpy(dest, text.c_str(), text.size() + 1);
  return true;
}

bool number(JsonObjectConst root, const char *key, uint32_t &dest,
            uint32_t minimum, uint32_t maximum, String &error) {
  if (!root.containsKey(key)) return true;
  if (!root[key].is<uint32_t>() || root[key].is<bool>()) return fail(error, key);
  uint32_t value = root[key].as<uint32_t>();
  if (value < minimum || value > maximum) return fail(error, key);
  dest = value;
  return true;
}

bool readBoolean(JsonObjectConst root, const char *key, bool &dest, String &error) {
  if (!root.containsKey(key)) return true;
  if (!root[key].is<bool>()) return fail(error, key);
  dest = root[key].as<bool>();
  return true;
}

bool objectType(JsonObjectConst root, const char *key, String &error) {
  return !root.containsKey(key) || root[key].is<JsonObjectConst>() || fail(error, key);
}

template <size_t N>
bool hasKeys(JsonObjectConst object, const char *const (&keys)[N]) {
  if (object.isNull()) return false;
  for (const char *key : keys) if (!object.containsKey(key)) return false;
  return true;
}

bool completeSnapshot(JsonObjectConst root, uint32_t schema) {
  const char *const networkKeys[] = {"mode", "ssid", "password", "fallback_seconds"};
  const char *const controlKeys[] = {"auto_mode", "start_delay_ms", "press_ms", "stop_press_ms",
      "verify_timeout_ms", "standby_filter_ms", "indicator_filter_ms", "off_confirm_ms",
      "restart_delay_ms", "retry_delay_ms", "max_attempts", "auto_on_boot", "allow_battery_start"};
  const char *const inputKeys[] = {"online_active_high", "battery_active_high", "standby_active_high"};
  const char *const securityKeys[] = {"password"};
  const char *const batteryKeys[] = {"enabled", "full_scale_mv", "low_warning_mv"};
  return hasKeys(root["network"], networkKeys) && hasKeys(root["ups"], controlKeys) &&
      hasKeys(root["inputs"], inputKeys) && hasKeys(root["security"], securityKeys) &&
      (schema == 1 || hasKeys(root["battery"], batteryKeys));
}
}

const char *networkModeName(NetworkMode mode) {
  switch (mode) {
    case NetworkMode::Router: return "router";
    case NetworkMode::Both: return "both";
    default: return "standalone";
  }
}

void settingsToJson(const Settings &s, JsonObject root, bool secrets) {
  JsonObject net = root.createNestedObject("network");
  net["mode"] = networkModeName(s.network_mode);
  net["ssid"] = s.ssid;
  net["has_password"] = s.wifi_password[0] != 0;
  net["ap_ssid"] = board::ApSsid;
  net["ap_ip"] = "192.168.7.1";
  net["fallback_seconds"] = s.fallback_seconds;
  if (secrets) net["password"] = s.wifi_password;
  JsonObject control = root.createNestedObject("ups");
  control["auto_mode"] = ups::autoModeName(s.control.auto_mode);
#define STORE_NUMBER(name) control[#name] = s.control.name
  STORE_NUMBER(start_delay_ms);
  STORE_NUMBER(press_ms);
  STORE_NUMBER(stop_press_ms);
  STORE_NUMBER(verify_timeout_ms);
  STORE_NUMBER(standby_filter_ms);
  STORE_NUMBER(indicator_filter_ms);
  STORE_NUMBER(off_confirm_ms);
  STORE_NUMBER(restart_delay_ms);
  STORE_NUMBER(retry_delay_ms);
  STORE_NUMBER(max_attempts);
  STORE_NUMBER(auto_on_boot);
  STORE_NUMBER(allow_battery_start);
#undef STORE_NUMBER
  JsonObject inputs = root.createNestedObject("inputs");
  inputs["online_active_high"] = s.online_active_high;
  inputs["battery_active_high"] = s.battery_active_high;
  inputs["standby_active_high"] = s.standby_active_high;
  JsonObject battery = root.createNestedObject("battery");
  battery["enabled"] = s.battery.enabled;
  battery["full_scale_mv"] = s.battery.full_scale_mv;
  battery["low_warning_mv"] = s.battery.low_warning_mv;
  JsonObject security = root.createNestedObject("security");
  security["username"] = board::Username;
  if (secrets) security["password"] = s.admin_password;
}

bool patchSettings(JsonObjectConst root, Settings &s, String &error) {
  if (root.isNull()) return fail(error, "JSON object");
  if (!objectType(root, "network", error) || !objectType(root, "ups", error) ||
      !objectType(root, "inputs", error) || !objectType(root, "security", error) ||
      !objectType(root, "battery", error)) return false;
  JsonObjectConst net = root["network"];
  if (net.containsKey("mode")) {
    const char *mode = net["mode"].as<const char *>();
    if (!mode) return fail(error, "network.mode");
    if (!strcmp(mode, "standalone")) s.network_mode = NetworkMode::Standalone;
    else if (!strcmp(mode, "router")) s.network_mode = NetworkMode::Router;
    else if (!strcmp(mode, "both")) s.network_mode = NetworkMode::Both;
    else return fail(error, "network.mode");
  }
  if (!copyText(net, "ssid", s.ssid, sizeof(s.ssid), error) ||
      !copyText(net, "password", s.wifi_password, sizeof(s.wifi_password), error)) return false;
  if (s.network_mode != NetworkMode::Standalone && !s.ssid[0]) return fail(error, "network.ssid");
  size_t passLength = strlen(s.wifi_password);
  if (passLength && passLength < 8) return fail(error, "network.password (8..63 bytes)");
  uint32_t fallback = s.fallback_seconds;
  if (!number(net, "fallback_seconds", fallback, 10, 120, error)) return false;
  s.fallback_seconds = static_cast<uint16_t>(fallback);

  JsonObjectConst control = root["ups"];
  if (control.containsKey("auto_mode")) {
    const char *mode = control["auto_mode"].as<const char *>();
    if (!mode) return fail(error, "ups.auto_mode");
    if (!strcmp(mode, "disabled")) s.control.auto_mode = ups::AutoMode::Disabled;
    else if (!strcmp(mode, "on_mains")) s.control.auto_mode = ups::AutoMode::OnMains;
    else if (!strcmp(mode, "keep_on")) s.control.auto_mode = ups::AutoMode::KeepOn;
    else return fail(error, "ups.auto_mode");
  }
#define NUMBER(name, low, high) if (!number(control, #name, s.control.name, low, high, error)) return false
  NUMBER(start_delay_ms, 1000, 600000);
  NUMBER(press_ms, 100, 10000);
  NUMBER(stop_press_ms, 100, 10000);
  NUMBER(verify_timeout_ms, 1000, 120000);
  NUMBER(standby_filter_ms, 50, 5000);
  NUMBER(indicator_filter_ms, 50, 5000);
  NUMBER(off_confirm_ms, 300, 30000);
  NUMBER(restart_delay_ms, 1000, 600000);
  NUMBER(retry_delay_ms, 10000, 3600000);
#undef NUMBER
  if (s.control.off_confirm_ms < s.control.indicator_filter_ms) return fail(error, "off_confirm_ms >= indicator_filter_ms");
  if (s.control.verify_timeout_ms <= s.control.off_confirm_ms) return fail(error, "verify_timeout_ms > off_confirm_ms");
  uint32_t attempts = s.control.max_attempts;
  if (!number(control, "max_attempts", attempts, 1, 3, error)) return false;
  s.control.max_attempts = static_cast<uint8_t>(attempts);
  if (!readBoolean(control, "auto_on_boot", s.control.auto_on_boot, error) ||
      !readBoolean(control, "allow_battery_start", s.control.allow_battery_start, error)) return false;
  JsonObjectConst inputs = root["inputs"];
  if (!readBoolean(inputs, "online_active_high", s.online_active_high, error) ||
      !readBoolean(inputs, "battery_active_high", s.battery_active_high, error) ||
      !readBoolean(inputs, "standby_active_high", s.standby_active_high, error)) return false;
  JsonObjectConst battery = root["battery"];
  if (!readBoolean(battery, "enabled", s.battery.enabled, error) ||
      !number(battery, "full_scale_mv", s.battery.full_scale_mv, 10000, 100000, error) ||
      !number(battery, "low_warning_mv", s.battery.low_warning_mv, 0, 14000, error)) return false;
  if (s.battery.low_warning_mv && s.battery.low_warning_mv < 8000) {
    return fail(error, "battery.low_warning_mv (0 or 8000..14000)");
  }
  if (!copyText(root["security"], "password", s.admin_password, sizeof(s.admin_password), error)) return false;
  if (strlen(s.admin_password) < 8) return fail(error, "security.password (8..63 bytes)");
  return true;
}

bool SettingsStore::readFile(const char *path, Settings &settings) {
  File file = LittleFS.open(path, "r");
  if (!file || !file.size() || file.size() > MaxConfigBytes) return false;
  DynamicJsonDocument document(4096);
  DeserializationError parsed = deserializeJson(document, file);
  file.close();
  if (parsed || !document["schema"].is<uint32_t>() || document["schema"].is<bool>()) return false;
  const uint32_t schema = document["schema"].as<uint32_t>();
  if ((schema != 1 && schema != 2) || !completeSnapshot(document.as<JsonObjectConst>(), schema)) return false;
  Settings candidate;
  String error;
  if (!patchSettings(document.as<JsonObjectConst>(), candidate, error)) return false;
  // Firmware 1.0.x had no voltage input. Preserve all its settings, leave A0 off.
  if (schema == 1) candidate.battery = ups::BatteryConfig();
  settings = candidate;
  return true;
}

bool SettingsStore::begin(Settings &settings, String &message) {
  mounted_ = LittleFS.begin();
  if (!mounted_) {
    settings.control.auto_mode = ups::AutoMode::Disabled;
    message = F("Сховище недоступне; автозапуск вимкнено");
    return false;
  }
  if (readFile("/config.json", settings)) {
    healthy_ = true;
    message = F("Налаштування завантажено");
    return true;
  }
  if (readFile("/config.bak", settings)) {
    healthy_ = true;
    message = F("Налаштування відновлено з резервної копії");
    return true;
  }
  if (LittleFS.exists("/config.json") || LittleFS.exists("/config.bak")) {
    settings.control.auto_mode = ups::AutoMode::Disabled;
    message = F("Налаштування пошкоджено; автозапуск вимкнено. Збережіть їх знову");
    return false;
  }
  healthy_ = true;
  message = F("Перший запуск: стандартні налаштування");
  return true;
}

bool SettingsStore::save(const Settings &settings, String &error) {
  if (!mounted_) { error = F("Сховище недоступне"); return false; }
  DynamicJsonDocument document(4096);
  settingsToJson(settings, document.to<JsonObject>(), true);
  document["schema"] = 2;
  if (document.overflowed()) { error = F("Налаштування завеликі"); return false; }
  File file = LittleFS.open("/config.tmp", "w");
  if (!file) { error = F("Не вдалося відкрити файл налаштувань"); return false; }
  size_t expected = measureJson(document);
  size_t written = serializeJson(document, file);
  file.flush();
  file.close();
  Settings verified;
  if (written != expected || !readFile("/config.tmp", verified)) {
    LittleFS.remove("/config.tmp");
    error = F("Помилка перевірки запису налаштувань");
    return false;
  }
  // Keep one known-good configuration until the replacement is completely written.
  if (LittleFS.exists("/config.json")) {
    Settings previous;
    if (readFile("/config.json", previous)) {
      LittleFS.remove("/config.bak");
      if (!LittleFS.rename("/config.json", "/config.bak")) {
        error = F("Не вдалося зберегти резервну копію"); return false;
      }
    } else {
      LittleFS.remove("/config.json");
    }
  }
  if (!LittleFS.rename("/config.tmp", "/config.json")) {
    error = F("Не вдалося застосувати налаштування"); return false;
  }
  healthy_ = true;
  return true;
}
