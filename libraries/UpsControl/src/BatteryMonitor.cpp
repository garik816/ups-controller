#include "BatteryMonitor.h"

namespace ups {

BatteryMonitor::BatteryMonitor() { resetReadings(0); }

void BatteryMonitor::resetReadings(uint32_t now) {
  sum_ = 0;
  last_sample_at_ = now;
  window_started_at_ = now;
  published_at_ = now;
  voltage_mv_ = 0;
  raw_adc_ = 0;
  last_raw_ = 0;
  count_ = 0;
  sampled_ = false;
  published_ = false;
  window_saturated_ = false;
  low_ = false;
  result_state_ = BatteryState::WarmingUp;
}

void BatteryMonitor::begin(const BatteryConfig &config, uint32_t now) {
  configure(config, now);
}

void BatteryMonitor::configure(const BatteryConfig &config, uint32_t now) {
  config_ = config;
  resetReadings(now);
}

bool BatteryMonitor::sampleDue(uint32_t now) const {
  return config_.enabled &&
      (!sampled_ || uint32_t(now - last_sample_at_) >= SamplePeriodMs);
}

void BatteryMonitor::sample(uint16_t raw, uint32_t now) {
  if (!sampleDue(now)) return;

  // Do not mix measurements across a long pause (for example an OTA upload).
  // Also reject a slowly accumulated window that spans more than two seconds.
  if ((sampled_ && uint32_t(now - last_sample_at_) > StaleAfterMs) ||
      (count_ && uint32_t(now - window_started_at_) > StaleAfterMs)) {
    resetReadings(now);
  }
  if (!count_) window_started_at_ = now;
  last_sample_at_ = now;
  sampled_ = true;
  last_raw_ = raw > 1023 ? 1023 : raw;
  if (raw >= SaturationCode) window_saturated_ = true;
  sum_ += last_raw_;
  ++count_;
  if (count_ < SamplesPerReading) return;

  raw_adc_ = static_cast<uint16_t>((sum_ + SamplesPerReading / 2) / SamplesPerReading);
  constexpr uint32_t denominator = uint32_t(SamplesPerReading) * 1023;
  // Use the original sum, not the rounded ADC display value. The explicit wide
  // cast also prevents 16-bit arithmetic overflow on the ATmega328P.
  voltage_mv_ = static_cast<uint32_t>(
      (uint64_t(sum_) * config_.full_scale_mv + denominator / 2) / denominator);
  result_state_ = window_saturated_ ? BatteryState::Saturated :
      (raw_adc_ <= 1 ? BatteryState::NoSignal : BatteryState::Ready);
  if (result_state_ == BatteryState::Ready) {
    if (!config_.low_warning_mv) {
      low_ = false;
    } else if (!low_ && voltage_mv_ < config_.low_warning_mv) {
      low_ = true;
    } else if (low_ && uint64_t(voltage_mv_) >=
               uint64_t(config_.low_warning_mv) + WarningHysteresisMv) {
      low_ = false;
    }
  }
  published_at_ = now;
  published_ = true;
  sum_ = 0;
  count_ = 0;
  window_saturated_ = false;
}

BatterySnapshot BatteryMonitor::snapshot(uint32_t now) const {
  BatterySnapshot result = {};
  result.enabled = config_.enabled;
  result.state = BatteryState::Disabled;
  if (!config_.enabled) return result;
  result.raw_adc = raw_adc_;
  result.age_ms = published_ ? uint32_t(now - published_at_) :
      (sampled_ ? uint32_t(now - last_sample_at_) : 0);
  if (sampled_ && uint32_t(now - last_sample_at_) > StaleAfterMs) {
    result.state = BatteryState::Stale;
    return result;
  }
  // Even a short rail excursion invalidates the previous published voltage.
  if (window_saturated_) {
    result.raw_adc = last_raw_;
    result.saturated = true;
    result.state = BatteryState::Saturated;
    return result;
  }
  if (published_ && uint32_t(now - published_at_) > StaleAfterMs) {
    result.state = BatteryState::Stale;
    return result;
  }
  if (!published_) {
    result.state = BatteryState::WarmingUp;
    return result;
  }
  result.state = result_state_;
  result.saturated = result_state_ == BatteryState::Saturated;
  result.valid = result_state_ == BatteryState::Ready;
  if (result.valid) {
    result.voltage_mv = voltage_mv_;
    result.low = low_;
  }
  return result;
}

const char *batteryStateName(BatteryState state) {
  switch (state) {
    case BatteryState::Disabled: return "disabled";
    case BatteryState::WarmingUp: return "warming_up";
    case BatteryState::Ready: return "ok";
    case BatteryState::NoSignal: return "no_signal";
    case BatteryState::Saturated: return "overrange";
    case BatteryState::Stale: return "stale";
  }
  return "unknown";
}

bool batteryFullScaleFromReference(uint16_t raw, uint32_t reference_mv,
                                   uint32_t &out_full_scale_mv) {
  if (raw < 10 || raw >= BatteryMonitor::SaturationCode ||
      reference_mv < 8000 || reference_mv > 16000) return false;
  const uint32_t scale = static_cast<uint32_t>(
      (uint64_t(reference_mv) * 1023 + raw / 2) / raw);
  if (scale < 10000 || scale > 100000) return false;
  out_full_scale_mv = scale;
  return true;
}

}  // namespace ups
