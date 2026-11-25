#include <stdint.h>
#include <cstdlib>
#include <iostream>

#include "BatteryMonitor.h"

#define CHECK(expression) do { \
  if (!(expression)) { \
    std::cerr << __func__ << ':' << __LINE__ << " failed: " #expression << '\n'; \
    std::exit(1); \
  } \
} while (false)

struct Rig {
  ups::BatteryConfig config;
  ups::BatteryMonitor monitor;
  uint32_t now;

  explicit Rig(uint32_t scale = 62600, uint32_t warning = 0, uint32_t start = 0)
      : now(start) {
    config.enabled = true;
    config.full_scale_mv = scale;
    config.low_warning_mv = warning;
    monitor.begin(config, now);
  }
  void step(uint16_t raw, uint32_t advance = 50) {
    now += advance;
    monitor.sample(raw, now);
  }
  void window(uint16_t raw) {
    for (unsigned i = 0; i < 16; ++i) step(raw);
  }
  ups::BatterySnapshot status() const { return monitor.snapshot(now); }
};

void disabled_has_no_reading_or_sample_requests() {
  ups::BatteryMonitor monitor;
  CHECK(!monitor.sampleDue(0));
  monitor.sample(500, 0);
  CHECK(!monitor.snapshot(0).enabled);
  CHECK(!monitor.snapshot(0).valid);
  CHECK(monitor.snapshot(0).state == ups::BatteryState::Disabled);
  ups::BatteryConfig config;
  monitor.begin(config, 1234);
  CHECK(!monitor.sampleDue(0xFFFFFFFEU));
}

void complete_window_required_before_publication() {
  Rig r;
  CHECK(r.monitor.sampleDue(r.now));
  CHECK(r.status().state == ups::BatteryState::WarmingUp);
  for (unsigned i = 0; i < 15; ++i) r.step(200);
  CHECK(!r.status().valid);
  r.step(200);
  CHECK(r.status().valid);
  CHECK(r.status().raw_adc == 200);
  CHECK(r.status().voltage_mv == 12239);
  CHECK(r.status().age_ms == 0);
}

void sample_interval_prevents_bursts() {
  Rig r;
  r.monitor.sample(200, r.now);
  CHECK(!r.monitor.sampleDue(49));
  CHECK(r.monitor.sampleDue(50));
  for (unsigned i = 0; i < 32; ++i) r.monitor.sample(1023, 1);
  CHECK(r.status().state == ups::BatteryState::WarmingUp);
  CHECK(!r.status().saturated);
  for (unsigned i = 0; i < 15; ++i) r.step(200);
  CHECK(r.status().valid);
  CHECK(r.status().raw_adc == 200);
}

void nominal_board_scales_convert_correctly() {
  Rig nodemcu(62600);
  nodemcu.window(235);
  CHECK(nodemcu.status().voltage_mv == 14380);
  Rig bare(19000);
  bare.window(775);
  CHECK(bare.status().voltage_mv == 14394);
  Rig nano(21500);
  nano.window(685);
  CHECK(nano.status().voltage_mv == 14396);
}

void voltage_uses_fractional_average_not_display_rounding() {
  Rig r;
  for (unsigned i = 0; i < 8; ++i) r.step(200);
  for (unsigned i = 0; i < 8; ++i) r.step(201);
  CHECK(r.status().raw_adc == 201);
  CHECK(r.status().voltage_mv == 12269);
}

void wide_arithmetic_handles_large_scales() {
  Rig normal(80000);
  normal.window(1019);
  CHECK(normal.status().voltage_mv == 79687);
  Rig largest(UINT32_MAX);
  largest.window(1019);
  CHECK(largest.status().voltage_mv == 4278173679U);
}

void near_zero_is_unavailable_then_recovers() {
  Rig r;
  r.window(0);
  CHECK(!r.status().valid);
  CHECK(r.status().state == ups::BatteryState::NoSignal);
  CHECK(r.status().voltage_mv == 0);
  r.window(1);
  CHECK(r.status().state == ups::BatteryState::NoSignal);
  r.window(2);
  CHECK(r.status().valid);
  r.window(200);
  CHECK(r.status().valid);
  CHECK(r.status().voltage_mv == 12239);
}

void one_rail_sample_invalidates_immediately_and_full_window() {
  Rig r;
  r.window(200);
  CHECK(r.status().valid);
  r.step(1020);
  CHECK(!r.status().valid);
  CHECK(r.status().saturated);
  CHECK(r.status().state == ups::BatteryState::Saturated);
  CHECK(r.status().voltage_mv == 0);
  for (unsigned i = 0; i < 15; ++i) r.step(200);
  CHECK(r.status().saturated);
  for (unsigned i = 0; i < 15; ++i) r.step(200);
  CHECK(!r.status().valid);
  r.step(200);
  CHECK(r.status().valid);
  CHECK(!r.status().saturated);
}

void invalid_raw_code_is_clamped_and_rejected() {
  Rig r;
  r.window(UINT16_MAX);
  CHECK(r.status().saturated);
  CHECK(r.status().raw_adc == 1023);
  CHECK(r.status().voltage_mv == 0);
}

void warning_has_three_hundred_millivolt_hysteresis() {
  Rig r(20460, 11500);  // Exactly 20 mV per ADC code.
  r.window(575);
  CHECK(!r.status().low);  // At the threshold is not below it.
  r.window(574);
  CHECK(r.status().low);
  r.window(589);
  CHECK(r.status().low);
  r.window(590);
  CHECK(!r.status().low);
  r.window(580);
  CHECK(!r.status().low);
  r.window(574);
  CHECK(r.status().low);
}

void threshold_zero_disables_warning() {
  Rig r;
  r.window(2);
  CHECK(r.status().valid);
  CHECK(!r.status().low);
}

void configure_clears_previous_samples_and_warning() {
  Rig r(20460, 11500);
  r.window(500);
  CHECK(r.status().low);
  r.config.full_scale_mv = 21500;
  r.monitor.configure(r.config, r.now);
  CHECK(!r.status().valid);
  CHECK(r.status().state == ups::BatteryState::WarmingUp);
  CHECK(!r.status().low);
  r.window(685);
  CHECK(r.status().valid);
  CHECK(r.status().voltage_mv == 14396);
  r.config.enabled = false;
  r.monitor.configure(r.config, r.now);
  CHECK(!r.monitor.sampleDue(r.now + 100));
  CHECK(r.status().state == ups::BatteryState::Disabled);
  r.config.enabled = true;
  r.monitor.configure(r.config, r.now);
  CHECK(!r.status().valid);
}

void stale_values_are_hidden_and_gap_requires_new_window() {
  Rig r;
  r.window(200);
  CHECK(r.monitor.snapshot(r.now + 2000).valid);
  const ups::BatterySnapshot stale = r.monitor.snapshot(r.now + 2001);
  CHECK(!stale.valid);
  CHECK(stale.state == ups::BatteryState::Stale);
  CHECK(stale.voltage_mv == 0);
  CHECK(stale.age_ms == 2001);
  r.step(235, 2001);
  CHECK(r.status().state == ups::BatteryState::WarmingUp);
  for (unsigned i = 0; i < 15; ++i) r.step(235);
  CHECK(r.status().valid);
  CHECK(r.status().voltage_mv == 14380);
}

void fresh_partial_samples_do_not_refresh_old_published_values() {
  Rig r;
  r.window(200);
  r.step(235, 999);
  r.step(235, 999);
  CHECK(r.status().valid);
  r.step(235, 999);
  CHECK(r.status().state == ups::BatteryState::Stale);
  r.step(235, 999);
  CHECK(r.status().state == ups::BatteryState::WarmingUp);
  for (unsigned i = 0; i < 15; ++i) r.step(235);
  CHECK(r.status().voltage_mv == 14380);
}

void fresh_rail_sample_overrides_old_average_but_it_also_expires() {
  Rig r;
  r.window(200);
  r.step(200, 1000);
  r.step(200, 1000);
  r.step(1020, 1000);
  CHECK(r.status().state == ups::BatteryState::Saturated);
  CHECK(r.status().saturated);
  CHECK(!r.status().valid);
  CHECK(r.status().voltage_mv == 0);
  const ups::BatterySnapshot expired = r.monitor.snapshot(r.now + 2001);
  CHECK(expired.state == ups::BatteryState::Stale);
  CHECK(!expired.saturated && !expired.valid);
  CHECK(expired.voltage_mv == 0);
}

void sampling_and_staleness_survive_millis_wrap() {
  Rig r(62600, 0, UINT32_MAX - 400);
  r.window(235);
  CHECK(r.now < 500);
  CHECK(r.status().valid);
  CHECK(r.status().voltage_mv == 14380);
  CHECK(!r.monitor.sampleDue(r.now + 49));
  CHECK(r.monitor.sampleDue(r.now + 50));
  CHECK(r.monitor.snapshot(r.now + 2001).state == ups::BatteryState::Stale);
}

void calibration_matches_nominal_circuits() {
  uint32_t scale = 0;
  CHECK(ups::batteryFullScaleFromReference(235, 14400, scale));
  CHECK(scale == 62686);
  CHECK(ups::batteryFullScaleFromReference(775, 14400, scale));
  CHECK(scale == 19008);
  CHECK(ups::batteryFullScaleFromReference(685, 14400, scale));
  CHECK(scale == 21505);
  CHECK(ups::batteryFullScaleFromReference(800, 8000, scale));
  CHECK(scale == 10230);
  CHECK(ups::batteryFullScaleFromReference(205, 16000, scale));
  CHECK(scale == 79844);
}

void calibration_rejects_unsafe_or_unusable_inputs_without_mutation() {
  const uint16_t raw[] = {0, 9, 1020, 1023, UINT16_MAX, 200, 200, 819, 204, 10};
  const uint32_t reference[] = {12000, 12000, 12000, 12000, 12000,
      7999, 16001, 8000, 16000, 8000};
  for (unsigned i = 0; i < sizeof(raw) / sizeof(raw[0]); ++i) {
    uint32_t scale = 62600;
    CHECK(!ups::batteryFullScaleFromReference(raw[i], reference[i], scale));
    CHECK(scale == 62600);
  }
}

int main() {
  disabled_has_no_reading_or_sample_requests();
  complete_window_required_before_publication();
  sample_interval_prevents_bursts();
  nominal_board_scales_convert_correctly();
  voltage_uses_fractional_average_not_display_rounding();
  wide_arithmetic_handles_large_scales();
  near_zero_is_unavailable_then_recovers();
  one_rail_sample_invalidates_immediately_and_full_window();
  invalid_raw_code_is_clamped_and_rejected();
  warning_has_three_hundred_millivolt_hysteresis();
  threshold_zero_disables_warning();
  configure_clears_previous_samples_and_warning();
  stale_values_are_hidden_and_gap_requires_new_window();
  fresh_partial_samples_do_not_refresh_old_published_values();
  fresh_rail_sample_overrides_old_average_but_it_also_expires();
  sampling_and_staleness_survive_millis_wrap();
  calibration_matches_nominal_circuits();
  calibration_rejects_unsafe_or_unusable_inputs_without_mutation();
  std::cout << "18 battery measurement/calibration tests passed\n";
}
