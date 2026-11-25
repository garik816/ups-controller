#pragma once

#include <Arduino.h>

namespace board {
constexpr uint8_t RelayPin = 5;     // NodeMCU D1
constexpr uint8_t OnlinePin = 4;    // NodeMCU D2
constexpr uint8_t BatteryPin = 14;  // NodeMCU D5
constexpr uint8_t StandbyPin = 12;  // NodeMCU D6
constexpr uint8_t RelayOn = LOW;
constexpr uint8_t RelayOff = HIGH;
constexpr char ApSsid[] = "UPS_wifi";
constexpr char ApPassword[] = "1234567890";
constexpr char Username[] = "admin";
constexpr char Hostname[] = "ups-controller";
}
