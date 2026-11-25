#include <Arduino.h>
#include <FirmwareVersion.h>
#include <UpsController.h>
#include <BatteryMonitor.h>
#include "BoardConfig.h"

ups::Controller controller;
ups::BatteryMonitor batteryMonitor;

void eventLog(void *, ups::Event event) {
  Serial.print(millis());
  Serial.print(F(" UPS: "));
  Serial.println(ups::eventName(event));
}

void printBattery() {
  const ups::BatterySnapshot battery = batteryMonitor.snapshot(millis());
  Serial.print(F("Battery state="));
  Serial.print(ups::batteryStateName(battery.state));
  Serial.print(F(" raw="));
  Serial.print(battery.raw_adc);
  if (battery.valid) {
    Serial.print(F(" mV="));
    Serial.print(battery.voltage_mv);
    Serial.print(F(" low="));
    Serial.print(battery.low);
  }
  Serial.println();
}

void printStatus() {
  const ups::Snapshot s = controller.snapshot(millis());
  Serial.print(F("UPS Controller "));
  Serial.print(F(UPS_FIRMWARE_VERSION));
  Serial.print(F(" state="));
  Serial.print(ups::stateName(s.state));
  Serial.print(F(" phase="));
  Serial.print(ups::phaseName(s.phase));
  Serial.print(F(" result="));
  Serial.print(ups::resultName(s.result));
  Serial.print(F(" mains="));
  Serial.print(s.mains);
  Serial.print(F(" relay="));
  Serial.print(s.relay);
  Serial.print(F(" hold="));
  Serial.print(s.auto_hold);
  Serial.print(F(" attempts="));
  Serial.println(s.attempts);
  printBattery();
}

void setup() {
  // A hardware pull-up on a compatible relay input is still required to keep
  // the relay inactive during bootloader execution and controller reset.
  digitalWrite(RELAY_PIN, RELAY_OFF);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(ONLINE_PIN, INPUT);
  pinMode(BATTERY_PIN, INPUT);
  pinMode(STANDBY_PIN, INPUT);
  Serial.begin(115200);
  controller.setEventCallback(eventLog, nullptr);
  ups::Config config;
  // Both variants use the same defaults and state machine. To reproduce the
  // legacy 300-ms OFF check, set off_confirm_ms to 300 (less blink immunity).
  controller.begin(config, millis());
  ups::BatteryConfig batteryConfig;
  batteryConfig.enabled = BATTERY_MEASUREMENT_ENABLED;
  batteryConfig.full_scale_mv = BATTERY_FULL_SCALE_MV;
  batteryConfig.low_warning_mv = BATTERY_LOW_WARNING_MV;
  batteryMonitor.begin(batteryConfig, millis());
  Serial.println(F("Commands: ? status; v battery; s start; o stop; r restart; c cancel; a resume"));
}

void loop() {
  const uint32_t now = millis();
  controller.tick(now,
      (digitalRead(STANDBY_PIN) == HIGH) == STANDBY_ACTIVE_HIGH,
      (digitalRead(ONLINE_PIN) == HIGH) == ONLINE_ACTIVE_HIGH,
      (digitalRead(BATTERY_PIN) == HIGH) == BATTERY_ACTIVE_HIGH);
  digitalWrite(RELAY_PIN, controller.relayActive() ? RELAY_ON : RELAY_OFF);
  if (batteryMonitor.sampleDue(now)) {
    batteryMonitor.sample(static_cast<uint16_t>(analogRead(BATTERY_ADC_PIN)), now);
  }
  if (!Serial.available()) return;
  const char input = static_cast<char>(Serial.read());
  ups::Command action = ups::Command::Cancel;
  bool recognized = true;
  switch (input) {
    case '?': printStatus(); return;
    case 'v': printBattery(); return;
    case 's': action = ups::Command::Start; break;
    case 'o': action = ups::Command::Stop; break;
    case 'r': action = ups::Command::Restart; break;
    case 'c': action = ups::Command::Cancel; break;
    case 'a': action = ups::Command::Resume; break;
    default: recognized = false; break;
  }
  if (recognized) {
    const ups::CommandStatus status = controller.command(action, now);
    digitalWrite(RELAY_PIN, controller.relayActive() ? RELAY_ON : RELAY_OFF);
    Serial.print(F("Command: "));
    Serial.println(ups::commandStatusName(status));
  }
}
