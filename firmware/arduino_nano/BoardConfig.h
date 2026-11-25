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

// Optional battery measurement: battery + -> 33k -> A0; A0 -> 10k -> GND;
// 100nF from A0 to GND. Use only the verified common low-voltage ground.
// Keep disabled until the divider is installed. Do not connect battery +
// directly to A0. The circuit is intended for 0..16 V battery input.
constexpr uint8_t BATTERY_ADC_PIN = A0;
constexpr bool BATTERY_MEASUREMENT_ENABLED = false;
// Nominal AVCC reference 5000mV * (33k + 10k) / 10k = 21500mV.
// Calibrate against a multimeter; actual USB/regulator voltage affects AVCC.
constexpr uint32_t BATTERY_FULL_SCALE_MV = 21500;
constexpr uint32_t BATTERY_LOW_WARNING_MV = 0;  // 0 disables the warning.
