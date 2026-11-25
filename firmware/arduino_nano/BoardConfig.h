#pragma once

// Original Arduino Nano / ATmega328P wiring. External circuits must establish
// definite logic levels on all three inputs; INPUT_PULLUP changes the meaning.
constexpr uint8_t RELAY_PIN = 8;
constexpr uint8_t ONLINE_PIN = 9;
constexpr uint8_t BATTERY_PIN = 10;
constexpr uint8_t STANDBY_PIN = 11;
constexpr uint8_t RELAY_ON = LOW;
constexpr uint8_t RELAY_OFF = HIGH;
constexpr bool ONLINE_ACTIVE_HIGH = true;
constexpr bool BATTERY_ACTIVE_HIGH = true;
constexpr bool STANDBY_ACTIVE_HIGH = true;
