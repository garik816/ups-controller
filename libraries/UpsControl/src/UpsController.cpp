#include "UpsController.h"

namespace ups {
namespace {
bool elapsed(uint32_t now, uint32_t since, uint32_t duration) {
  return static_cast<uint32_t>(now - since) >= duration;
}
uint32_t remaining(uint32_t now, uint32_t since, uint32_t duration) {
  const uint32_t used = static_cast<uint32_t>(now - since);
  return used >= duration ? 0 : duration - used;
}
}  // namespace

Config::Config()
    : auto_mode(AutoMode::OnMains), start_delay_ms(10000), press_ms(2000),
      stop_press_ms(2000), verify_timeout_ms(15000), standby_filter_ms(300),
      indicator_filter_ms(300), off_confirm_ms(2000), restart_delay_ms(10000),
      retry_delay_ms(60000), max_attempts(1), auto_on_boot(true),
      allow_battery_start(false) {}

Controller::Controller()
    : phase_(Phase::Idle), result_(Result::None), callback_(0),
      callback_context_(0), eligible_at_(0), phase_at_(0), retry_at_(0),
      off_at_(0), first_mains_observation_(true), running_known_(false),
      running_(false), off_tracking_(false), relay_(false),
      automatic_action_(false), restarting_(false), suspended_(false),
      attempts_logged_(false) {
  latch_ = Latch{false, false, false, false, 0};
  mains_filter_.reset();
  online_filter_.reset();
  battery_filter_.reset();
}

void Controller::Filter::reset() {
  initialized = false;
  previous = false;
  value = false;
  valid = false;
  changed_at = 0;
}

void Controller::Filter::update(bool raw, uint32_t now, uint32_t period) {
  if (!initialized || raw != previous) {
    initialized = true;
    previous = raw;
    changed_at = now;
    valid = false;
  }
  if (!valid && elapsed(now, changed_at, period)) {
    value = raw;
    valid = true;
  }
}

void Controller::setEventCallback(EventCallback callback, void *context) {
  callback_ = callback;
  callback_context_ = context;
}

void Controller::emit(Event event) {
  if (callback_) callback_(callback_context_, event);
}

void Controller::begin(const Config &config, uint32_t now, const Latch *restored) {
  config_ = config;
  latch_ = Latch{false, false, false, false, 0};
  if (restored) {
    latch_ = *restored;
    if (latch_.attempts > 3) latch_.attempts = 3;
  }
  mains_filter_.reset();
  resetIndicators();
  first_mains_observation_ = true;
  relay_ = false;
  automatic_action_ = false;
  restarting_ = false;
  suspended_ = false;
  attempts_logged_ = false;
  eligible_at_ = now;
  phase_at_ = now;
  retry_at_ = now;
  phase_ = Phase::Idle;
  result_ = Result::None;
  emit(restored ? Event::Restored : Event::Ready);
}

void Controller::resetIndicators() {
  online_filter_.reset();
  battery_filter_.reset();
  running_known_ = false;
  running_ = false;
  off_tracking_ = false;
}

void Controller::updateIndicators(uint32_t now, bool raw_online, bool raw_battery) {
  online_filter_.update(raw_online, now, config_.indicator_filter_ms);
  battery_filter_.update(raw_battery, now, config_.indicator_filter_ms);

  if (raw_online || raw_battery) {
    off_tracking_ = false;
  } else if (!off_tracking_) {
    off_tracking_ = true;
    off_at_ = now;
  }

  const bool confirmed_on =
      (online_filter_.valid && online_filter_.value) ||
      (battery_filter_.valid && battery_filter_.value);
  const bool confirmed_off =
      online_filter_.valid && !online_filter_.value &&
      battery_filter_.valid && !battery_filter_.value && off_tracking_ &&
      elapsed(now, off_at_, config_.off_confirm_ms);

  if (confirmed_on) {
    running_known_ = true;
    running_ = true;
  } else if (confirmed_off) {
    if (running_known_ && running_) eligible_at_ = now;
    running_known_ = true;
    running_ = false;
  } else if (!running_ && (raw_online || raw_battery)) {
    // An ON transition may mean the UPS is starting itself. Immediately
    // withdraw the previous OFF decision while that transition is filtered.
    running_known_ = false;
  }
}

void Controller::observeMains(uint32_t now) {
  if (!mains_filter_.valid) return;
  const bool mains = mains_filter_.value;
  const bool first = first_mains_observation_;
  const bool early_manual_hold = first && !latch_.cycle_known && latch_.auto_hold;
  const bool same_restored_cycle = first && latch_.cycle_known &&
                                   latch_.mains_present == mains;
  const bool active_manual_hold = busy() && latch_.auto_hold;
  if (!first && latch_.cycle_known && latch_.mains_present == mains) return;

  first_mains_observation_ = false;
  if (same_restored_cycle) {
    eligible_at_ = now;
    if (!mains) {
      // A confirmed absence of mains is always a valid reset boundary.
      latch_.cycle_handled = false;
      latch_.auto_hold = false;
      latch_.attempts = 0;
    }
    return;
  }

  latch_.cycle_known = true;
  latch_.mains_present = mains;
  latch_.cycle_handled = false;
  latch_.auto_hold = false;
  latch_.attempts = 0;
  attempts_logged_ = false;
  eligible_at_ = now;
  retry_at_ = now;

  if (!mains) {
    if (busy()) {
      relay_ = false;
      automatic_action_ = false;
      restarting_ = false;
      result_ = Result::MainsLost;
    }
    phase_ = suspended_ ? Phase::Paused : Phase::Idle;
    emit(Event::MainsLost);
  } else {
    // A mains return while a manual stop/restart is in progress must not erase
    // its RTC-visible suppression before shutdown has been verified.
    if (active_manual_hold ||
        (first && (!config_.auto_on_boot || early_manual_hold))) {
      latch_.cycle_handled = true;
      latch_.auto_hold = true;
    }
    if (!busy()) phase_ = suspended_ ? Phase::Paused : Phase::Waiting;
    emit(Event::MainsPresent);
  }
}

void Controller::startPress(uint32_t now, bool automatic) {
  automatic_action_ = automatic;
  latch_.cycle_handled = true;
  latch_.auto_hold = false;
  if (automatic) {
    ++latch_.attempts;
    retry_at_ = now;
  }
  relay_ = true;
  phase_ = Phase::PressStart;
  phase_at_ = now;
  result_ = Result::None;
  emit(automatic ? Event::AutoStart : Event::ManualStart);
}

void Controller::stopPress(uint32_t now, bool restart) {
  automatic_action_ = false;
  restarting_ = restart;
  latch_.cycle_handled = true;
  latch_.auto_hold = true;
  relay_ = true;
  phase_ = Phase::PressStop;
  phase_at_ = now;
  result_ = Result::None;
  emit(Event::StopRequested);
}

void Controller::finish(Result result, Event event) {
  relay_ = false;
  phase_ = Phase::Finished;
  result_ = result;
  automatic_action_ = false;
  restarting_ = false;
  emit(event);
}

bool Controller::startPowerAvailable() const {
  return (mains_filter_.valid && mains_filter_.value) ||
         config_.allow_battery_start;
}

void Controller::runAutomatic(uint32_t now) {
  if (suspended_) {
    phase_ = Phase::Paused;
    return;
  }
  if (!mains_filter_.valid || !mains_filter_.value ||
      config_.auto_mode == AutoMode::Disabled || latch_.auto_hold) {
    phase_ = Phase::Idle;
    return;
  }
  if (config_.auto_mode == AutoMode::OnMains && latch_.cycle_handled) {
    phase_ = Phase::Finished;
    return;
  }
  if (config_.auto_mode == AutoMode::KeepOn && running_known_ && running_) {
    if (!latch_.cycle_handled) {
      latch_.cycle_handled = true;
      result_ = Result::AlreadyOn;
      emit(Event::AlreadyOn);
    }
    phase_ = Phase::Finished;
    return;
  }
  const uint8_t allowed = config_.auto_mode == AutoMode::OnMains ?
                          1 : config_.max_attempts;
  if (latch_.attempts >= allowed) {
    if (!attempts_logged_) {
      attempts_logged_ = true;
      if (result_ == Result::None || result_ == Result::AlreadyOn)
        result_ = Result::AttemptsExhausted;
      emit(Event::AttemptsExhausted);
    }
    phase_ = Phase::Finished;
    return;
  }
  if (!elapsed(now, eligible_at_, config_.start_delay_ms)) {
    phase_ = Phase::Waiting;
    return;
  }
  if (latch_.attempts > 0 &&
      !elapsed(now, retry_at_, config_.retry_delay_ms)) {
    phase_ = Phase::Cooldown;
    return;
  }
  if (!running_known_) {
    phase_ = Phase::Checking;
    return;
  }
  if (running_) {
    latch_.cycle_handled = true;
    finish(Result::AlreadyOn, Event::AlreadyOn);
  } else {
    restarting_ = false;
    startPress(now, true);
  }
}

void Controller::tick(uint32_t now, bool raw_mains, bool raw_online,
                      bool raw_battery) {
  mains_filter_.update(raw_mains, now, config_.standby_filter_ms);
  updateIndicators(now, raw_online, raw_battery);
  observeMains(now);

  switch (phase_) {
    case Phase::PressStart:
      if (elapsed(now, phase_at_, config_.press_ms)) {
        relay_ = false;
        phase_ = Phase::VerifyStart;
        phase_at_ = now;
        resetIndicators();
        emit(Event::RelayReleased);
      }
      return;
    case Phase::PressStop:
      if (elapsed(now, phase_at_, config_.stop_press_ms)) {
        relay_ = false;
        phase_ = Phase::VerifyStop;
        phase_at_ = now;
        resetIndicators();
        emit(Event::RelayReleased);
      }
      return;
    case Phase::VerifyStart:
      if (running_known_ && running_) {
        finish(Result::Started, Event::StartConfirmed);
      } else if (elapsed(now, phase_at_, config_.verify_timeout_ms)) {
        if (!automatic_action_) latch_.auto_hold = true;
        finish(Result::StartFailed, Event::StartFailed);
      }
      return;
    case Phase::VerifyStop:
      if (running_known_ && !running_) {
        latch_.auto_hold = true;
        if (restarting_) {
          relay_ = false;
          phase_ = Phase::RestartWait;
          phase_at_ = now;
          result_ = Result::Stopped;
          emit(Event::StopConfirmed);
          emit(Event::RestartWaiting);
        } else {
          finish(Result::Stopped, Event::StopConfirmed);
        }
      } else if (elapsed(now, phase_at_, config_.verify_timeout_ms)) {
        latch_.auto_hold = true;
        finish(Result::StopFailed, Event::StopFailed);
      }
      return;
    case Phase::RestartWait:
      if (!elapsed(now, phase_at_, config_.restart_delay_ms)) return;
      if (!startPowerAvailable()) {
        latch_.auto_hold = true;
        finish(Result::MainsLost, Event::MainsLost);
      } else if (running_known_ && running_) {
        // The UPS turned on itself. A second button press could switch it off.
        latch_.auto_hold = false;
        finish(Result::AlreadyOn, Event::AlreadyOn);
      } else if (running_known_) {
        startPress(now, false);
      } else if (elapsed(now, phase_at_, config_.restart_delay_ms +
                         config_.verify_timeout_ms)) {
        latch_.auto_hold = true;
        finish(Result::StartFailed, Event::StartFailed);
      }
      return;
    default:
      runAutomatic(now);
      return;
  }
}

CommandStatus Controller::command(Command action, uint32_t now) {
  if (action == Command::Cancel) {
    relay_ = false;
    latch_.cycle_handled = true;
    latch_.auto_hold = true;
    finish(Result::Cancelled, Event::Cancelled);
    return CommandStatus::Accepted;
  }
  if (suspended_) return CommandStatus::Suspended;
  if (busy()) return CommandStatus::Busy;
  if (action == Command::Resume) {
    if (!mains_filter_.valid) return CommandStatus::InputsUnknown;
    latch_.auto_hold = false;
    latch_.cycle_handled = false;
    latch_.attempts = 0;
    attempts_logged_ = false;
    eligible_at_ = now;
    retry_at_ = now;
    result_ = Result::None;
    phase_ = Phase::Waiting;
    emit(Event::AutoResumed);
    return CommandStatus::Accepted;
  }
  if (!mains_filter_.valid || !running_known_) return CommandStatus::InputsUnknown;
  if (action == Command::Start) {
    if (running_) return CommandStatus::AlreadyOn;
    if (!startPowerAvailable()) return CommandStatus::MainsRequired;
    restarting_ = false;
    startPress(now, false);
    return CommandStatus::Accepted;
  }
  if (!running_) return CommandStatus::AlreadyOff;
  if (!(online_filter_.valid && online_filter_.value) &&
      !(battery_filter_.valid && battery_filter_.value))
    return CommandStatus::InputsUnknown;
  if (action == Command::Restart && !startPowerAvailable())
    return CommandStatus::MainsRequired;
  stopPress(now, action == Command::Restart);
  return CommandStatus::Accepted;
}

bool Controller::configure(const Config &config, uint32_t now) {
  if (busy() || suspended_) return false;
  config_ = config;
  mains_filter_.reset();
  resetIndicators();
  first_mains_observation_ = true;
  eligible_at_ = now;
  retry_at_ = now;
  phase_ = Phase::Idle;
  result_ = Result::None;
  relay_ = false;
  emit(Event::ConfigChanged);
  return true;
}

bool Controller::setSuspended(bool suspended, uint32_t now) {
  if (suspended && busy()) return false;
  if (suspended_ == suspended) return true;
  suspended_ = suspended;
  relay_ = false;
  eligible_at_ = now;
  phase_ = suspended ? Phase::Paused : Phase::Idle;
  emit(suspended ? Event::Suspended : Event::Resumed);
  return true;
}

bool Controller::busy() const {
  return phase_ == Phase::PressStart || phase_ == Phase::VerifyStart ||
         phase_ == Phase::PressStop || phase_ == Phase::VerifyStop ||
         phase_ == Phase::RestartWait;
}

bool Controller::relayActive() const { return relay_; }
const Config &Controller::config() const { return config_; }
Latch Controller::latch() const { return latch_; }

State Controller::state() const {
  if (!running_known_) return State::Unknown;
  if (!running_) return State::Off;
  const bool online = online_filter_.valid && online_filter_.value;
  const bool battery = battery_filter_.valid && battery_filter_.value;
  if (online && !battery) return State::Online;
  if (battery && !online) return State::Battery;
  return State::Running;
}

Snapshot Controller::snapshot(uint32_t now) const {
  uint32_t left = 0;
  switch (phase_) {
    case Phase::Waiting: left = remaining(now, eligible_at_, config_.start_delay_ms); break;
    case Phase::PressStart: left = remaining(now, phase_at_, config_.press_ms); break;
    case Phase::PressStop: left = remaining(now, phase_at_, config_.stop_press_ms); break;
    case Phase::VerifyStart:
    case Phase::VerifyStop: left = remaining(now, phase_at_, config_.verify_timeout_ms); break;
    case Phase::RestartWait: left = remaining(now, phase_at_, config_.restart_delay_ms); break;
    case Phase::Cooldown: left = remaining(now, retry_at_, config_.retry_delay_ms); break;
    default: break;
  }
  const Snapshot value = {
    state(), phase_, result_, busy(), relay_,
    mains_filter_.valid && mains_filter_.value,
    online_filter_.valid && online_filter_.value,
    battery_filter_.valid && battery_filter_.value,
    mains_filter_.valid && running_known_ &&
      (!running_ || (online_filter_.valid && online_filter_.value) ||
                    (battery_filter_.valid && battery_filter_.value)),
    latch_.auto_hold,
    latch_.cycle_handled, latch_.attempts, left
  };
  return value;
}

const char *autoModeName(AutoMode mode) {
  switch (mode) {
    case AutoMode::Disabled: return "disabled";
    case AutoMode::OnMains: return "on_mains";
    case AutoMode::KeepOn: return "keep_on";
  }
  return "disabled";
}
const char *stateName(State value) {
  switch (value) {
    case State::Unknown: return "unknown";
    case State::Off: return "off";
    case State::Online: return "online";
    case State::Battery: return "battery";
    case State::Running: return "running";
  }
  return "unknown";
}
const char *phaseName(Phase value) {
  switch (value) {
    case Phase::Idle: return "idle";
    case Phase::Waiting: return "waiting";
    case Phase::Checking: return "checking";
    case Phase::PressStart: return "press_start";
    case Phase::VerifyStart: return "verify_start";
    case Phase::PressStop: return "press_stop";
    case Phase::VerifyStop: return "verify_stop";
    case Phase::RestartWait: return "restart_wait";
    case Phase::Cooldown: return "cooldown";
    case Phase::Finished: return "finished";
    case Phase::Paused: return "paused";
  }
  return "idle";
}
const char *resultName(Result value) {
  switch (value) {
    case Result::None: return "none";
    case Result::AlreadyOn: return "already_on";
    case Result::Started: return "started";
    case Result::StartFailed: return "start_failed";
    case Result::Stopped: return "stopped";
    case Result::StopFailed: return "stop_failed";
    case Result::Cancelled: return "cancelled";
    case Result::MainsLost: return "mains_lost";
    case Result::AttemptsExhausted: return "attempts_exhausted";
  }
  return "none";
}
const char *commandStatusName(CommandStatus value) {
  switch (value) {
    case CommandStatus::Accepted: return "accepted";
    case CommandStatus::Busy: return "busy";
    case CommandStatus::InputsUnknown: return "inputs_unknown";
    case CommandStatus::AlreadyOn: return "already_on";
    case CommandStatus::AlreadyOff: return "already_off";
    case CommandStatus::MainsRequired: return "mains_required";
    case CommandStatus::Suspended: return "suspended";
  }
  return "inputs_unknown";
}
const char *eventName(Event value) {
  switch (value) {
    case Event::Ready: return "controller_ready";
    case Event::MainsPresent: return "mains_present";
    case Event::MainsLost: return "mains_lost";
    case Event::AlreadyOn: return "ups_already_on";
    case Event::AutoStart: return "automatic_start";
    case Event::ManualStart: return "manual_start";
    case Event::StopRequested: return "stop_requested";
    case Event::RelayReleased: return "button_released";
    case Event::StartConfirmed: return "start_confirmed";
    case Event::StartFailed: return "start_not_confirmed";
    case Event::StopConfirmed: return "stop_confirmed";
    case Event::StopFailed: return "stop_not_confirmed";
    case Event::RestartWaiting: return "restart_waiting";
    case Event::Cancelled: return "action_cancelled";
    case Event::AutoResumed: return "automation_resumed";
    case Event::AttemptsExhausted: return "automatic_attempt_limit";
    case Event::Restored: return "cycle_state_restored";
    case Event::ConfigChanged: return "configuration_applied";
    case Event::Suspended: return "automation_suspended";
    case Event::Resumed: return "automation_unsuspended";
  }
  return "unknown_event";
}

}  // namespace ups
