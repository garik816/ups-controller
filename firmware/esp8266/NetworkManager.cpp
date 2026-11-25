#include "NetworkManager.h"

#include "BoardConfig.h"

namespace {
const IPAddress ApIp(192, 168, 7, 1);
const IPAddress ApMask(255, 255, 255, 0);
uint32_t ipNumber(const IPAddress &ip) {
  return (uint32_t(ip[0]) << 24) | (uint32_t(ip[1]) << 16) |
         (uint32_t(ip[2]) << 8) | uint32_t(ip[3]);
}
}

void NetworkManager::log(const char *level, const char *message) {
  if (logger_) logger_(context_, level, message);
}

void NetworkManager::setAp(bool enabled) {
  if (enabled == ap_active_) return;
  if (enabled) {
    WiFi.enableAP(true);
    const bool configured = WiFi.softAPConfig(ApIp, ApIp, ApMask);
    ap_active_ = configured && WiFi.softAP(board::ApSsid, board::ApPassword, 1, false, 4);
    log(ap_active_ ? "info" : "error", ap_active_
        ? "Точку UPS_wifi увімкнено: 192.168.7.1" : "Помилка запуску точки UPS_wifi");
  } else {
    WiFi.softAPdisconnect(true);
    ap_active_ = false;
    log("info", "Точку доступу вимкнено; керування через роутер");
  }
}

void NetworkManager::begin(const Settings &settings, uint32_t now) {
  settings_ = settings;
  WiFi.persistent(false);
  WiFi.setAutoConnect(false);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false);
  if (scan_requested_) WiFi.scanDelete();
  scan_requested_ = false;
  scan_station_enabled_ = false;
  if (ap_active_) setAp(false);
  WiFi.mode(settings_.network_mode == NetworkMode::Standalone ? WIFI_AP : WIFI_AP_STA);
  WiFi.hostname(board::Hostname);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  connected_ = false;
  fallback_ = false;
  conflict_ = false;
  disconnected_at_ = now;
  connected_at_ = now;
  last_reconnect_at_ = now;
  last_ap_attempt_at_ = now;
  // Every boot and network reconfiguration offers the requested recovery AP.
  setAp(true);
  if (settings_.network_mode != NetworkMode::Standalone) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(settings_.ssid, settings_.wifi_password);
    log("info", "Підключення до роутера розпочато");
  }
}

bool NetworkManager::overlappingSubnet() const {
  const uint32_t routerMask = ipNumber(WiFi.subnetMask());
  const uint32_t commonMask = routerMask & ipNumber(ApMask);
  return (ipNumber(WiFi.localIP()) & commonMask) == (ipNumber(ApIp) & commonMask);
}

void NetworkManager::tick(uint32_t now) {
  if (scan_station_enabled_ && !scanning()) {
    WiFi.enableSTA(false);
    scan_station_enabled_ = false;
  }
  const bool needAp = settings_.network_mode != NetworkMode::Router || fallback_ || conflict_;
  if (needAp && !ap_active_ && uint32_t(now - last_ap_attempt_at_) >= 10000) {
    last_ap_attempt_at_ = now;
    setAp(true);
  }
  if (settings_.network_mode == NetworkMode::Standalone || conflict_) return;
  bool up = WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
  if (up && overlappingSubnet()) {
    conflict_ = true;
    connected_ = false;
    fallback_ = true;
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false);
    setAp(true);
    log("error", "Підмережа роутера перетинається з 192.168.7.0/24. Залишено UPS_wifi");
    return;
  }
  if (up != connected_) {
    connected_ = up;
    if (up) {
      connected_at_ = now;
      fallback_ = false;
      char message[96];
      snprintf(message, sizeof(message), "Підключено до роутера: http://%s", WiFi.localIP().toString().c_str());
      log("info", message);
    } else {
      disconnected_at_ = now;
      log("warning", "З'єднання з роутером втрачено");
    }
  }
  if (connected_) {
    if (settings_.network_mode == NetworkMode::Router &&
        uint32_t(now - connected_at_) >= 10000) setAp(false);
  } else {
    if (uint32_t(now - disconnected_at_) >= uint32_t(settings_.fallback_seconds) * 1000) {
      fallback_ = true;
      setAp(true);
    }
    if (!scanning() && uint32_t(now - last_reconnect_at_) >= 30000) {
      last_reconnect_at_ = now;
      WiFi.reconnect();
    }
  }
}

void NetworkManager::status(JsonObject o) const {
  o["mode"] = networkModeName(settings_.network_mode);
  o["ap_active"] = ap_active_;
  o["ap_ssid"] = board::ApSsid;
  o["ap_ip"] = "192.168.7.1";
  o["ap_clients"] = ap_active_ ? WiFi.softAPgetStationNum() : 0;
  o["sta_connected"] = connected_;
  o["sta_ssid"] = settings_.ssid;
  o["sta_ip"] = connected_ ? WiFi.localIP().toString() : String("0.0.0.0");
  o["rssi"] = connected_ ? WiFi.RSSI() : 0;
  o["fallback"] = fallback_;
  o["subnet_conflict"] = conflict_;
}

bool NetworkManager::scanning() const {
  return scan_requested_ && WiFi.scanComplete() == WIFI_SCAN_RUNNING;
}

bool NetworkManager::startScan() {
  if (scanning()) return false;
  WiFi.scanDelete();
  // scanNetworks requires station capability, even in standalone mode.
  if (settings_.network_mode == NetworkMode::Standalone) {
    WiFi.enableSTA(true);
    scan_station_enabled_ = true;
  }
  int result = WiFi.scanNetworks(true, true);
  scan_requested_ = result == WIFI_SCAN_RUNNING || result >= 0;
  return scan_requested_;
}

void NetworkManager::scanResults(JsonObject object) {
  int count = scan_requested_ ? WiFi.scanComplete() : WIFI_SCAN_FAILED;
  object["running"] = count == WIFI_SCAN_RUNNING;
  JsonArray networks = object.createNestedArray("networks");
  if (count < 0) return;
  for (int i = 0; i < count && networks.size() < 24; ++i) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool duplicate = false;
    for (JsonObject existing : networks) {
      if (ssid == existing["ssid"].as<const char *>()) { duplicate = true; break; }
    }
    if (duplicate) continue;
    JsonObject network = networks.createNestedObject();
    network["ssid"] = ssid;
    network["rssi"] = WiFi.RSSI(i);
    network["secure"] = WiFi.encryptionType(i) != ENC_TYPE_NONE;
  }
}
