#pragma once

#include <stdint.h>

namespace ups {

// full_scale_mv is the mathematical scale at ADC code 1023. It is NOT the
// permitted battery input voltage; the documented hardware is for 0..16 V.
struct BatteryConfig {
  bool enabled = false;
  uint32_t full_scale_mv = 62600;
  uint32_t low_warning_mv = 0;  // 0 disables the notification threshold.
};

enum class BatteryState : uint8_t {
  Disabled, WarmingUp, Ready, NoSignal, Saturated, Stale
};

struct BatterySnapshot {
  bool enabled;
  bool valid;
  uint16_t raw_adc;
  uint32_t voltage_mv;  // Only meaningful when valid is true.
  bool low;
  bool saturated;
  BatteryState state;
  uint32_t age_ms;
};

class BatteryMonitor {
public:
  static constexpr uint32_t SamplePeriodMs = 50;
  static constexpr uint8_t SamplesPerReading = 16;
  static constexpr uint32_t StaleAfterMs = 2000;
  static constexpr uint16_t SaturationCode = 1020;
  static constexpr uint32_t WarningHysteresisMv = 300;

  BatteryMonitor();
  void begin(const BatteryConfig &config, uint32_t now);
  void configure(const BatteryConfig &config, uint32_t now);
  bool sampleDue(uint32_t now) const;
  // Calls arriving earlier than SamplePeriodMs are ignored. No ADC access or
  // blocking delay happens here; the board reads its ADC only when sampleDue.
  void sample(uint16_t raw, uint32_t now);
  BatterySnapshot snapshot(uint32_t now) const;
  const BatteryConfig &config() const { return config_; }

private:
  BatteryConfig config_;
  uint32_t sum_;
  uint32_t last_sample_at_;
  uint32_t window_started_at_;
  uint32_t published_at_;
  uint32_t voltage_mv_;
  uint16_t raw_adc_;
  uint16_t last_raw_;
  uint8_t count_;
  bool sampled_;
  bool published_;
  bool window_saturated_;
  bool low_;
  BatteryState result_state_;

  void resetReadings(uint32_t now);
};

const char *batteryStateName(BatteryState state);

// Calibrate from a stable multimeter reading. Leaves out_full_scale_mv intact
// on rejection. Input limits describe this project's 12 V battery circuit.
bool batteryFullScaleFromReference(uint16_t raw, uint32_t reference_mv,
                                   uint32_t &out_full_scale_mv);

}  // namespace ups
