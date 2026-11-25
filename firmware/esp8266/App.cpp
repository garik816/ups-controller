#include "App.h"

#include <Updater.h>
#include <FirmwareVersion.h>
extern "C" {
#include <user_interface.h>
}
#include "BoardConfig.h"
#include "WebUi.h"

namespace {
// eboot owns user RTC bytes 0..127 for OTA handoff commands.
constexpr uint32_t RtcWordOffset = 32;

void abortUpdate() {
  if (!Update.isRunning()) return;
  if (Update.isFinished()) {
    // ESP8266 3.1.2 has no abort(). A write beyond remaining()==0 only
    // sets UPDATE_ERROR_SPACE; it does not read/write the supplied byte.
    // This prevents end(false) from committing a fully received but cancelled upload.
    uint8_t extra = 0;
    Update.write(&extra, 1);
  }
  Update.end(false);
}
const char *eventMessage(ups::Event event) {
  switch (event) {
    case ups::Event::Ready: return "Контролер UPS готовий";
    case ups::Event::MainsPresent: return "Живлення від мережі з'явилося";
    case ups::Event::MainsLost: return "Живлення від мережі зникло";
    case ups::Event::AlreadyOn: return "UPS вже працює; кнопку не натискаємо";
    case ups::Event::AutoStart: return "Автоматичний запуск UPS";
    case ups::Event::ManualStart: return "Ручний запуск UPS";
    case ups::Event::StopRequested: return "Надіслано команду вимкнення UPS";
    case ups::Event::RelayReleased: return "Кнопку живлення відпущено";
    case ups::Event::StartConfirmed: return "Запуск UPS підтверджено";
    case ups::Event::StartFailed: return "Запуск UPS не підтверджено";
    case ups::Event::StopConfirmed: return "Вимкнення UPS підтверджено";
    case ups::Event::StopFailed: return "Вимкнення UPS не підтверджено";
    case ups::Event::RestartWaiting: return "Пауза перед повторним увімкненням UPS";
    case ups::Event::Cancelled: return "Операцію скасовано, кнопку відпущено";
    case ups::Event::AutoResumed: return "Автоматику відновлено вручну";
    case ups::Event::AttemptsExhausted: return "Ліміт спроб автозапуску вичерпано";
    case ups::Event::Restored: return "Стан циклу мережі відновлено після скидання";
    case ups::Event::ConfigChanged: return "Налаштування UPS застосовано";
    case ups::Event::Suspended: return "Автоматика тимчасово призупинена";
    case ups::Event::Resumed: return "Автоматика продовжує роботу";
  }
  return ups::eventName(event);
}

const char *commandError(ups::CommandStatus status) {
  switch (status) {
    case ups::CommandStatus::Busy: return "UPS вже виконує операцію";
    case ups::CommandStatus::InputsUnknown: return "Стан індикаторів ще не підтверджено";
    case ups::CommandStatus::AlreadyOn: return "UPS вже увімкнений";
    case ups::CommandStatus::AlreadyOff: return "UPS вже вимкнений";
    case ups::CommandStatus::MainsRequired: return "Немає мережі; запуск від батареї заборонений у налаштуваннях";
    case ups::CommandStatus::Suspended: return "Керування призупинене для обслуговування";
    default: return "Команду відхилено";
  }
}

bool sameNetwork(const Settings &a, const Settings &b) {
  return a.network_mode == b.network_mode && !strcmp(a.ssid, b.ssid) &&
      !strcmp(a.wifi_password, b.wifi_password) && a.fallback_seconds == b.fallback_seconds;
}

bool sameControl(const Settings &a, const Settings &b) {
#define SAME_CONTROL(name) if (a.control.name != b.control.name) return false
  SAME_CONTROL(auto_mode);
  SAME_CONTROL(start_delay_ms);
  SAME_CONTROL(press_ms);
  SAME_CONTROL(stop_press_ms);
  SAME_CONTROL(verify_timeout_ms);
  SAME_CONTROL(standby_filter_ms);
  SAME_CONTROL(indicator_filter_ms);
  SAME_CONTROL(off_confirm_ms);
  SAME_CONTROL(restart_delay_ms);
  SAME_CONTROL(retry_delay_ms);
  SAME_CONTROL(max_attempts);
  SAME_CONTROL(auto_on_boot);
  SAME_CONTROL(allow_battery_start);
#undef SAME_CONTROL
  return a.online_active_high == b.online_active_high &&
      a.battery_active_high == b.battery_active_high &&
      a.standby_active_high == b.standby_active_high;
}
}

void App::setup() {
  digitalWrite(board::RelayPin, board::RelayOff);
  pinMode(board::RelayPin, OUTPUT);
  pinMode(board::OnlinePin, INPUT);
  pinMode(board::BatteryPin, INPUT);
  pinMode(board::StandbyPin, INPUT);
  Serial.begin(115200);
  Serial.println();
  Serial.print(F("UPS Controller ESP8266 v"));
  Serial.println(UPS_FIRMWARE_VERSION);
  controller_.setEventCallback(coreEvent, this);
  network_.setLogger(networkEvent, this);
  String storageMessage;
  store_.begin(settings_, storageMessage);
  log(store_.healthy() ? "info" : "error", storageMessage.c_str());
  battery_monitor_.begin(settings_.battery, millis());
  ups::Latch retained;
  const bool restored = restoreRtc(retained);
  controller_.begin(settings_.control, millis(), restored ? &retained : nullptr);
  for (uint8_t i = 0; i < 4; ++i) {
    snprintf(csrf_token_ + i * 8, 9, "%08lx", static_cast<unsigned long>(os_random()));
  }
  network_.begin(settings_, millis());
  server_.collectHeaders("X-UPS-Token", "Content-Type");
  routes();
  server_.begin();
  log("info", "Веб-керування готове");
}

void App::loop() {
  updateController();
  updateBattery();
  uint32_t now = millis();
  if (reboot_pending_ && uint32_t(now - reboot_requested_at_) >= 1500) {
    digitalWrite(board::RelayPin, board::RelayOff);
    persistRtc();
    ESP.restart();
    return;
  }
  if (network_pending_ && !controller_.busy() && !uploading_ &&
      uint32_t(now - network_apply_at_) >= 1000) {
    network_pending_ = false;
    network_.begin(settings_, now);
  }
  network_.tick(now);
  server_.handleClient();
  updateController();
  serialCommands();
  yield();
}

void App::serialCommands() {
  // Bounded input processing keeps a noisy serial cable from starving control.
  for (uint8_t handled = 0; handled < 32 && Serial.available(); ++handled) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\r') continue;
    if (character != '\n') {
      if (static_cast<size_t>(serial_length_) + 1 < sizeof(serial_line_)) serial_line_[serial_length_++] = character;
      else serial_overflow_ = true;
      continue;
    }
    serial_line_[serial_length_] = 0;
    serial_length_ = 0;
    if (serial_overflow_) { serial_overflow_ = false; Serial.println(F("ERR command too long")); continue; }
    if (!strcmp(serial_line_, "help")) {
      Serial.println(F("Commands: status, network-reset, help (newline required)"));
    } else if (!strcmp(serial_line_, "status")) {
      ups::Snapshot s = controller_.snapshot(millis());
      Serial.printf("UPS %s phase=%s mains=%u online=%u battery=%u relay=%u\n", ups::stateName(s.state), ups::phaseName(s.phase), s.mains, s.online, s.battery, s.relay);
      Serial.print(F("AP: http://192.168.7.1; router: "));
      Serial.println(WiFi.localIP());
      const ups::BatterySnapshot battery = battery_monitor_.snapshot(millis());
      Serial.printf("Battery voltage: %s raw=%u", ups::batteryStateName(battery.state), battery.raw_adc);
      if (battery.valid) Serial.printf(" mV=%lu low=%u", static_cast<unsigned long>(battery.voltage_mv), battery.low);
      Serial.println();
    } else if (!strcmp(serial_line_, "network-reset")) {
      if (controller_.busy() || uploading_ || reboot_pending_ || network_.scanning()) {
        Serial.println(F("ERR busy; retry after UPS operation")); continue;
      }
      Settings candidate = settings_;
      candidate.network_mode = NetworkMode::Standalone;
      candidate.ssid[0] = 0;
      candidate.wifi_password[0] = 0;
      strlcpy(candidate.admin_password, "1234567890", sizeof(candidate.admin_password));
      String problem;
      if (!store_.save(candidate, problem)) { Serial.println(problem); continue; }
      settings_ = candidate;
      network_pending_ = true;
      network_apply_at_ = millis();
      log("warning", "Мережу та пароль admin відновлено через USB; налаштування UPS збережено");
    } else if (serial_line_[0]) {
      Serial.println(F("ERR unknown command; type help"));
    }
  }
}

void App::updateController() {
  controller_.tick(millis(),
      (digitalRead(board::StandbyPin) == HIGH) == settings_.standby_active_high,
      (digitalRead(board::OnlinePin) == HIGH) == settings_.online_active_high,
      (digitalRead(board::BatteryPin) == HIGH) == settings_.battery_active_high);
  // Persist the attempt ledger before an actual relay edge is emitted.
  persistRtc();
  setRelayFromController();
}

void App::updateBattery() {
  if (uploading_) return;
  const uint32_t now = millis();
  if (battery_monitor_.sampleDue(now)) {
    battery_monitor_.sample(static_cast<uint16_t>(analogRead(A0)), now);
  }
  const ups::BatterySnapshot battery = battery_monitor_.snapshot(now);
  if (!battery.enabled) {
    battery_low_ = false;
    battery_overrange_ = false;
    return;
  }
  if (battery.saturated && !battery_overrange_) {
    log("warning", "АЦП батареї перевантажено; перевірте дільник напруги");
  }
  battery_overrange_ = battery.saturated;
  if (battery.valid && battery.low != battery_low_) {
    log(battery.low ? "warning" : "info", battery.low
        ? "Напруга батареї нижча за заданий поріг"
        : "Напруга батареї відновилася вище порога");
    battery_low_ = battery.low;
  }
}

void App::setRelayFromController() {
  ups::Snapshot s = controller_.snapshot(millis());
  if (s.relay && !requested_relay_) {
    requested_relay_ = true;
    relay_cutoff_ = false;
    const uint32_t maximum = s.phase == ups::Phase::PressStop ? settings_.control.stop_press_ms : settings_.control.press_ms;
    const uint32_t remaining = s.remaining_ms > 0 && s.remaining_ms <= maximum ? s.remaining_ms : maximum;
    // SDK timer releases the physical output even if a slow HTTP request is still
    // being parsed. The state machine also releases it on the normal loop path.
    relay_timer_.once_ms(remaining, [this]() {
      digitalWrite(board::RelayPin, board::RelayOff);
      relay_cutoff_ = true;
    });
    digitalWrite(board::RelayPin, board::RelayOn);
  } else if (!s.relay) {
    digitalWrite(board::RelayPin, board::RelayOff);
    if (requested_relay_) relay_timer_.detach();
    requested_relay_ = false;
    relay_cutoff_ = false;
  }
}

void App::log(const char *level, const char *message) {
  LogEntry &entry = logs_[log_head_];
  entry.id = next_log_id_++;
  entry.uptime_ms = millis();
  strlcpy(entry.level, level, sizeof(entry.level));
  strlcpy(entry.message, message, sizeof(entry.message));
  log_head_ = (log_head_ + 1) % LogCapacity;
  if (log_count_ < LogCapacity) ++log_count_;
  Serial.printf("[%lu] %s: %s\n", static_cast<unsigned long>(entry.uptime_ms), entry.level, entry.message);
}

void App::coreEvent(void *context, ups::Event event) {
  const bool warning = event == ups::Event::StartFailed || event == ups::Event::StopFailed ||
      event == ups::Event::AttemptsExhausted;
  static_cast<App *>(context)->log(warning ? "warning" : "info", eventMessage(event));
}

void App::networkEvent(void *context, const char *level, const char *message) {
  static_cast<App *>(context)->log(level, message);
}

bool App::tokenMatches() const {
  return server_.header("X-UPS-Token") == csrf_token_;
}

bool App::authenticate(bool mutation) {
  server_.sendHeader("Cache-Control", "no-store");
  if (!server_.authenticate(board::Username, settings_.admin_password)) {
    server_.requestAuthentication(BASIC_AUTH, "UPS Controller");
    return false;
  }
  if (mutation && !tokenMatches()) {
    error(403, "Сесію оновлено. Перезавантажте сторінку");
    return false;
  }
  return true;
}

bool App::idleForMaintenance() {
  updateController();
  if (controller_.busy() || uploading_ || reboot_pending_ || network_pending_ || network_.scanning()) {
    error(409, "Зачекайте завершення поточної операції");
    return false;
  }
  return true;
}

bool App::jsonBody(DynamicJsonDocument &document) {
  String contentType = server_.header("Content-Type");
  if (!contentType.startsWith("application/json")) {
    error(400, "Потрібен Content-Type: application/json"); return false;
  }
  const String &body = server_.arg("plain");
  if (body.length() > 3072) { error(413, "Запит завеликий"); return false; }
  if (deserializeJson(document, body) || !document.is<JsonObject>()) {
    error(400, "Некоректний JSON"); return false;
  }
  return true;
}

void App::sendJson(JsonDocument &document, int status) {
  if (document.overflowed()) { error(500, "Недостатньо пам'яті для відповіді"); return; }
  String payload;
  payload.reserve(measureJson(document) + 1);
  serializeJson(document, payload);
  server_.sendHeader("Cache-Control", "no-store");
  server_.sendHeader("X-Content-Type-Options", "nosniff");
  server_.send(status, "application/json; charset=utf-8", payload);
}

void App::success(const char *message) {
  DynamicJsonDocument document(512);
  document["ok"] = true;
  document["message"] = message;
  sendJson(document);
}

void App::error(int status, const char *message) {
  DynamicJsonDocument document(512);
  document["ok"] = false;
  document["error"] = message;
  String payload;
  serializeJson(document, payload);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(status, "application/json; charset=utf-8", payload);
}

void App::sendStatus() {
  if (!authenticate()) return;
  DynamicJsonDocument document(3072);
  document["version"] = UPS_FIRMWARE_VERSION;
  document["csrf_token"] = csrf_token_;
  document["uptime_ms"] = millis();
  document["free_heap"] = ESP.getFreeHeap();
  document["reset_reason"] = ESP.getResetReason();
  document["storage_ok"] = store_.healthy();
  document["rtc_ok"] = rtc_ok_;
  document["updating"] = uploading_;
  ups::Snapshot s = controller_.snapshot(millis());
  JsonObject state = document.createNestedObject("ups");
  state["state"] = ups::stateName(s.state);
  state["phase"] = ups::phaseName(s.phase);
  state["result"] = ups::resultName(s.result);
  state["busy"] = s.busy || uploading_ || reboot_pending_;
  state["relay"] = s.relay && !relay_cutoff_;
  state["mains"] = s.mains;
  state["online"] = s.online;
  state["battery"] = s.battery;
  state["inputs_valid"] = s.inputs_valid;
  state["auto_hold"] = s.auto_hold;
  state["cycle_handled"] = s.cycle_handled;
  state["attempts"] = s.attempts;
  state["remaining_ms"] = s.remaining_ms;
  const ups::BatterySnapshot reading = battery_monitor_.snapshot(millis());
  JsonObject battery = document.createNestedObject("battery");
  battery["enabled"] = reading.enabled;
  battery["valid"] = reading.valid;
  battery["state"] = ups::batteryStateName(reading.state);
  battery["low"] = reading.valid && reading.low;
  battery["saturated"] = reading.saturated;
  battery["full_scale_mv"] = settings_.battery.full_scale_mv;
  battery["low_warning_mv"] = settings_.battery.low_warning_mv;
  const char *readingState = ups::batteryStateName(reading.state);
  if (reading.enabled && strcmp(readingState, "warming_up") && strcmp(readingState, "stale")) {
    battery["raw_adc"] = reading.raw_adc;
  } else {
    battery["raw_adc"] = nullptr;
  }
  if (reading.valid) battery["voltage_mv"] = reading.voltage_mv;
  else battery["voltage_mv"] = nullptr;
  network_.status(document.createNestedObject("network"));
  sendJson(document);
}

void App::sendConfig() {
  if (!authenticate()) return;
  DynamicJsonDocument document(3072);
  settingsToJson(settings_, document.to<JsonObject>(), false);
  sendJson(document);
}

void App::saveConfig() {
  if (!authenticate(true) || !idleForMaintenance()) return;
  DynamicJsonDocument document(4096);
  if (!jsonBody(document)) return;
  Settings candidate = settings_;
  String problem;
  if (!patchSettings(document.as<JsonObjectConst>(), candidate, problem)) {
    error(400, problem.c_str()); return;
  }
  const bool polarityChanged = candidate.online_active_high != settings_.online_active_high ||
      candidate.battery_active_high != settings_.battery_active_high ||
      candidate.standby_active_high != settings_.standby_active_high;
  if (polarityChanged && candidate.control.auto_mode != ups::AutoMode::Disabled) {
    error(400, "Для зміни полярності спочатку вимкніть автоматичний режим. Перевірте сигнали перед його ввімкненням");
    return;
  }
  if (!store_.save(candidate, problem)) { error(500, problem.c_str()); return; }
  const bool networkChanged = !sameNetwork(settings_, candidate) || network_.hasConflict();
  const bool passwordChanged = strcmp(settings_.admin_password, candidate.admin_password) != 0;
  const bool controlChanged = !sameControl(settings_, candidate);
  const bool batteryChanged = settings_.battery.enabled != candidate.battery.enabled ||
      settings_.battery.full_scale_mv != candidate.battery.full_scale_mv ||
      settings_.battery.low_warning_mv != candidate.battery.low_warning_mv;
  // Voltage/network-only edits must not restart UPS debounce or autostart timers.
  if (controlChanged) controller_.configure(candidate.control, millis());
  settings_ = candidate;
  if (batteryChanged) {
    battery_monitor_.configure(settings_.battery, millis());
    battery_low_ = false;
    battery_overrange_ = false;
  }
  persistRtc();
  if (networkChanged) {
    network_pending_ = true;
    network_apply_at_ = millis();
  }
  log("info", "Налаштування збережено");
  success(passwordChanged ? "Збережено. Для наступного запиту введіть новий пароль admin"
                         : networkChanged ? "Збережено. Мережеві налаштування застосовуються"
                                          : "Налаштування збережено");
}

void App::calibrateBattery() {
  if (!authenticate(true) || !idleForMaintenance()) return;
  DynamicJsonDocument document(256);
  if (!jsonBody(document)) return;
  if (!document["reference_mv"].is<uint32_t>() || document["reference_mv"].is<bool>()) {
    error(400, "Введіть вимір мультиметра в мілівольтах (8000..16000)"); return;
  }
  const ups::BatterySnapshot reading = battery_monitor_.snapshot(millis());
  if (!reading.enabled || !reading.valid || reading.saturated) {
    error(409, "Увімкніть вимірювання та дочекайтеся придатного показу АЦП"); return;
  }
  uint32_t scale = 0;
  if (!ups::batteryFullScaleFromReference(reading.raw_adc, document["reference_mv"].as<uint32_t>(), scale)) {
    error(400, "Не вдалося калібрувати: перевірте вимір мультиметра, дільник і показ АЦП"); return;
  }
  Settings candidate = settings_;
  candidate.battery.full_scale_mv = scale;
  String problem;
  if (!store_.save(candidate, problem)) { error(500, problem.c_str()); return; }
  settings_ = candidate;
  battery_monitor_.configure(settings_.battery, millis());
  battery_low_ = false;
  battery_overrange_ = false;
  log("info", "Вимірювання батареї відкалібровано за мультиметром");
  success("Калібрування збережено. Оновлений показ з'явиться після усереднення");
}

void App::sendLogs() {
  if (!authenticate()) return;
  DynamicJsonDocument document(8192);
  JsonArray events = document.createNestedArray("events");
  for (uint8_t i = 0; i < log_count_; ++i) {
    const uint8_t index = (log_head_ + LogCapacity - log_count_ + i) % LogCapacity;
    const LogEntry &entry = logs_[index];
    JsonObject item = events.createNestedObject();
    item["id"] = entry.id;
    item["uptime_ms"] = entry.uptime_ms;
    item["level"] = entry.level;
    item["message"] = entry.message;
  }
  sendJson(document);
}

void App::action() {
  if (!authenticate(true)) return;
  DynamicJsonDocument document(256);
  if (!jsonBody(document)) return;
  const char *name = document["action"].as<const char *>();
  if (!name) { error(400, "Не вказано команду"); return; }
  ups::Command command;
  if (!strcmp(name, "start")) command = ups::Command::Start;
  else if (!strcmp(name, "stop")) command = ups::Command::Stop;
  else if (!strcmp(name, "restart")) command = ups::Command::Restart;
  else if (!strcmp(name, "cancel")) command = ups::Command::Cancel;
  else if (!strcmp(name, "resume")) command = ups::Command::Resume;
  else { error(400, "Невідома команда"); return; }
  if (command != ups::Command::Cancel &&
      (uploading_ || reboot_pending_ || network_pending_ || network_.scanning())) {
    error(409, "Зачекайте завершення обслуговування або пошуку Wi-Fi"); return;
  }
  updateController();
  ups::CommandStatus status = controller_.command(command, millis());
  if (status != ups::CommandStatus::Accepted) { error(409, commandError(status)); return; }
  persistRtc();
  setRelayFromController();
  success("Команду прийнято; перевірте результат у стані UPS");
}

void App::routes() {
  server_.on("/", HTTP_GET, [this]() {
    if (!authenticate()) return;
    server_.sendHeader("Content-Encoding", "gzip");
    server_.sendHeader("X-Frame-Options", "DENY");
    server_.sendHeader("X-Content-Type-Options", "nosniff");
    server_.sendHeader("Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'");
    server_.send_P(200, PSTR("text/html; charset=utf-8"), reinterpret_cast<PGM_P>(UPS_WEB_GZIP), UPS_WEB_GZIP_SIZE);
  });
  server_.on("/favicon.ico", HTTP_GET, [this]() { server_.send(204); });
  server_.on("/api/status", HTTP_GET, [this]() { sendStatus(); });
  server_.on("/api/config", HTTP_GET, [this]() { sendConfig(); });
  server_.on("/api/config", HTTP_POST, [this]() { saveConfig(); });
  server_.on("/api/battery/calibrate", HTTP_POST, [this]() { calibrateBattery(); });
  server_.on("/api/action", HTTP_POST, [this]() { action(); });
  server_.on("/api/logs", HTTP_GET, [this]() { sendLogs(); });
  server_.on("/api/scan", HTTP_POST, [this]() {
    if (!authenticate(true) || !idleForMaintenance()) return;
    if (network_.startScan()) success("Пошук мереж розпочато");
    else error(409, "Не вдалося розпочати пошук мереж");
  });
  server_.on("/api/scan", HTTP_GET, [this]() {
    if (!authenticate()) return;
    DynamicJsonDocument document(4096);
    network_.scanResults(document.to<JsonObject>());
    sendJson(document);
  });
  server_.on("/api/reboot", HTTP_POST, [this]() {
    if (!authenticate(true) || !idleForMaintenance()) return;
    controller_.setSuspended(true, millis());
    persistRtc();
    reboot_pending_ = true;
    reboot_requested_at_ = millis();
    success("Контролер перезавантажується; стан живлення UPS не змінюється");
  });
  server_.on("/api/update", HTTP_POST, [this]() { uploadFinished(); }, [this]() { uploadChunk(); });
  server_.onNotFound([this]() {
    if (authenticate()) error(404, "Сторінку не знайдено");
  });
}

void App::uploadChunk() {
  HTTPUpload &upload = server_.upload();
  if (upload.status == UPLOAD_FILE_START) {
    upload_started_ = false;
    upload_done_ = false;
    upload_error_ = "";
    upload_error_code_ = 400;
    if (!server_.authenticate(board::Username, settings_.admin_password)) {
      upload_error_ = F("Потрібна авторизація"); upload_error_code_ = 401; return;
    }
    if (!tokenMatches()) { upload_error_ = F("Недійсна сесія"); upload_error_code_ = 403; return; }
    if (controller_.busy() || network_pending_ || reboot_pending_ || network_.scanning() || uploading_) {
      upload_error_ = F("Контролер зайнятий"); upload_error_code_ = 409; return;
    }
    if (upload.name != "firmware" || !upload.filename.endsWith(".bin")) {
      upload_error_ = F("Потрібен файл прошивки ESP8266 .bin"); return;
    }
    if (!controller_.setSuspended(true, millis())) {
      upload_error_ = F("Не вдалося призупинити автоматику"); upload_error_code_ = 409; return;
    }
    uploading_ = true;
    persistRtc();
    digitalWrite(board::RelayPin, board::RelayOff);
    const uint32_t available = ESP.getFreeSketchSpace();
    if (available < 8192) { upload_error_ = F("Недостатньо місця для OTA"); upload_error_code_ = 500; return; }
    Update.runAsync(true);
    if (!Update.begin((available - 0x1000) & 0xFFFFF000, U_FLASH)) {
      upload_error_ = F("Не вдалося розпочати оновлення"); upload_error_code_ = 500; return;
    }
    upload_started_ = true;
    log("info", "Оновлення прошивки розпочато");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploading_) updateController();
    if (upload_started_ && upload_error_.isEmpty()) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        upload_error_ = F("Помилка запису або неправильний файл прошивки");
        upload_error_code_ = 500;
      }
    }
    yield();
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploading_) updateController();
    if (upload_started_ && upload_error_.isEmpty()) {
      upload_done_ = Update.end(true);
      if (!upload_done_) { upload_error_ = F("Перевірка прошивки не пройдена"); upload_error_code_ = 400; }
    } else if (upload_started_) {
      abortUpdate();
    }
    if (!upload_done_ && uploading_) {
      uploading_ = false;
      controller_.setSuspended(false, millis());
      log("error", "Оновлення не виконано; попередня прошивка залишається");
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (upload_started_) abortUpdate();
    upload_error_ = F("Передавання прошивки перервано");
    upload_done_ = false;
    if (uploading_) {
      uploading_ = false;
      controller_.setSuspended(false, millis());
      log("warning", "Оновлення перервано");
    }
  }
}

void App::uploadFinished() {
  if (!authenticate(true)) return;
  if (!upload_done_) {
    if (uploading_) {
      uploading_ = false;
      abortUpdate();
      controller_.setSuspended(false, millis());
    }
    error(upload_error_code_, upload_error_.isEmpty() ? "Файл прошивки не отримано" : upload_error_.c_str());
    return;
  }
  log("info", "Прошивку перевірено; перезавантаження контролера");
  reboot_pending_ = true;
  reboot_requested_at_ = millis();
  success("Прошивку оновлено. Контролер перезавантажується");
}

uint32_t App::rtcChecksum(const RtcRecord &record) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < sizeof(record) - sizeof(record.checksum); ++i) {
    hash = (hash ^ bytes[i]) * 16777619UL;
  }
  return hash;
}

bool App::restoreRtc(ups::Latch &latch) {
  // Power-on is a fresh cycle; RTC retention is for watchdog/software/external resets.
  if (ESP.getResetInfoPtr()->reason == REASON_DEFAULT_RST) return false;
  RtcRecord record;
  if (!ESP.rtcUserMemoryRead(RtcWordOffset, reinterpret_cast<uint32_t *>(&record), sizeof(record))) return false;
  if (record.magic != 0x55505331 || record.version != 1 ||
      record.checksum != rtcChecksum(record) || record.attempts > 3 || record.flags > 15) return false;
  rtc_cache_ = record;
  latch.cycle_known = record.flags & 1;
  latch.mains_present = record.flags & 2;
  latch.cycle_handled = record.flags & 4;
  latch.auto_hold = record.flags & 8;
  latch.attempts = static_cast<uint8_t>(record.attempts);
  return true;
}

void App::persistRtc() {
  ups::Latch latch = controller_.latch();
  RtcRecord record = {0x55505331, 1, 0, latch.attempts, 0};
  record.flags = (latch.cycle_known ? 1U : 0U) | (latch.mains_present ? 2U : 0U) |
      (latch.cycle_handled ? 4U : 0U) | (latch.auto_hold ? 8U : 0U);
  record.checksum = rtcChecksum(record);
  if (!memcmp(&record, &rtc_cache_, sizeof(record))) return;
  const bool written = ESP.rtcUserMemoryWrite(RtcWordOffset, reinterpret_cast<uint32_t *>(&record), sizeof(record));
  if (!written && rtc_ok_) log("warning", "Не вдалося зберегти стан циклу в RTC");
  rtc_ok_ = written;
  if (written) rtc_cache_ = record;
}
