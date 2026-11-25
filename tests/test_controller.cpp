#include <stdint.h>
#include <cstdlib>
#include <iostream>

#include "UpsController.h"

#define CHECK(expression) do { \
  if (!(expression)) { \
    std::cerr << __func__ << ':' << __LINE__ << " failed: " #expression << '\n'; \
    std::exit(1); \
  } \
} while (false)

ups::Config fastConfig() {
  ups::Config config;
  config.start_delay_ms = 100;
  config.press_ms = 50;
  config.stop_press_ms = 50;
  config.verify_timeout_ms = 200;
  config.standby_filter_ms = 10;
  config.indicator_filter_ms = 10;
  config.off_confirm_ms = 30;
  config.restart_delay_ms = 50;
  config.retry_delay_ms = 300;
  return config;
}

struct Rig {
  ups::Controller controller;
  uint32_t now;
  bool mains;
  bool online;
  bool battery;
  unsigned automatic_starts;
  unsigned manual_starts;
  unsigned stop_presses;
  unsigned releases;

  explicit Rig(const ups::Config &config = fastConfig(), uint32_t start = 0,
               bool initial_mains = true, bool initial_online = false,
               bool initial_battery = false, const ups::Latch *restored = 0)
      : now(start), mains(initial_mains), online(initial_online),
        battery(initial_battery), automatic_starts(0), manual_starts(0),
        stop_presses(0), releases(0) {
    controller.setEventCallback(onEvent, this);
    controller.begin(config, now, restored);
    tick();
  }

  static void onEvent(void *context, ups::Event event) {
    Rig &r = *static_cast<Rig *>(context);
    if (event == ups::Event::AutoStart) ++r.automatic_starts;
    if (event == ups::Event::ManualStart) ++r.manual_starts;
    if (event == ups::Event::StopRequested) ++r.stop_presses;
    if (event == ups::Event::RelayReleased) ++r.releases;
  }
  void tick() { controller.tick(now, mains, online, battery); }
  void advance(uint32_t duration) {
    for (uint32_t i = 0; i < duration; ++i) {
      ++now;
      tick();
    }
  }
  ups::Snapshot status() const { return controller.snapshot(now); }
  ups::CommandStatus command(ups::Command action) { return controller.command(action, now); }
};

void one_attempt_per_mains_cycle() {
  Rig r;
  r.advance(109);
  CHECK(!r.controller.relayActive());
  r.advance(1);
  CHECK(r.controller.relayActive());
  CHECK(r.status().attempts == 1);
  r.advance(50);
  CHECK(!r.controller.relayActive());
  CHECK(r.status().phase == ups::Phase::VerifyStart);
  r.advance(201);
  CHECK(r.status().result == ups::Result::StartFailed);
  r.advance(1000);
  CHECK(r.automatic_starts == 1);
  r.mains = false;
  r.advance(11);
  CHECK(r.status().attempts == 0);
  r.mains = true;
  r.advance(111);
  CHECK(r.automatic_starts == 2);
}

void already_running_never_pressed() {
  Rig r(fastConfig(), 0, true, true);
  r.advance(500);
  CHECK(r.status().state == ups::State::Online);
  CHECK(r.status().cycle_handled);
  CHECK(r.status().result == ups::Result::AlreadyOn);
  CHECK(r.automatic_starts == 0);
  r.online = false;
  r.advance(1000);
  CHECK(r.automatic_starts == 0);
  Rig b(fastConfig(), 0, true, false, true);
  b.advance(300);
  CHECK(b.status().state == ups::State::Battery);
  CHECK(b.automatic_starts == 0);
  Rig both(fastConfig(), 0, true, true, true);
  both.advance(300);
  CHECK(both.status().state == ups::State::Running);
  CHECK(both.automatic_starts == 0);
}

void unstable_mains_does_not_start() {
  Rig r(fastConfig(), 0, false);
  r.advance(20);
  for (unsigned i = 0; i < 30; ++i) {
    r.mains = !r.mains;
    r.advance(5);
  }
  CHECK(r.automatic_starts == 0);
  r.mains = true;
  r.advance(111);
  CHECK(r.automatic_starts == 1);
}

void blinking_indicators_do_not_look_off() {
  ups::Config config = fastConfig();
  config.auto_mode = ups::AutoMode::KeepOn;
  Rig r(config, 0, true, true);
  r.advance(150);
  for (unsigned i = 0; i < 10; ++i) {
    r.online = false;
    r.advance(20);
    CHECK(r.status().state != ups::State::Off);
    r.online = true;
    r.advance(15);
  }
  CHECK(r.automatic_starts == 0);
  r.online = false;
  r.advance(131);
  CHECK(r.automatic_starts == 1);
}

void on_transition_withdraws_old_off_decision() {
  Rig r;
  r.advance(109);
  CHECK(r.status().state == ups::State::Off);
  r.online = true;
  r.advance(2);
  CHECK(!r.controller.relayActive());
  CHECK(r.status().state == ups::State::Unknown);
  CHECK(r.command(ups::Command::Start) == ups::CommandStatus::InputsUnknown);
  r.advance(20);
  CHECK(r.automatic_starts == 0);
  CHECK(r.status().result == ups::Result::AlreadyOn);
}

void start_confirmation_is_measured_after_release() {
  Rig r;
  r.advance(110);
  r.online = true;
  r.advance(49);
  CHECK(r.status().state == ups::State::Online);
  r.advance(1);
  CHECK(r.status().state == ups::State::Unknown);
  r.online = false;
  r.advance(201);
  CHECK(r.status().result == ups::Result::StartFailed);
}

void mains_loss_releases_an_active_press() {
  Rig r;
  r.advance(110);
  CHECK(r.controller.relayActive());
  r.mains = false;
  r.tick();
  r.advance(9);
  CHECK(r.controller.relayActive());
  r.advance(1);
  CHECK(!r.controller.relayActive());
  CHECK(!r.controller.busy());
  CHECK(r.status().result == ups::Result::MainsLost);
  r.advance(1000);
  CHECK(r.automatic_starts == 1);
}

void commands_guard_state_and_concurrent_operations() {
  ups::Config config = fastConfig();
  config.auto_mode = ups::AutoMode::Disabled;
  Rig r(config);
  CHECK(r.command(ups::Command::Start) == ups::CommandStatus::InputsUnknown);
  r.advance(30);
  CHECK(r.command(ups::Command::Stop) == ups::CommandStatus::AlreadyOff);
  CHECK(r.command(ups::Command::Restart) == ups::CommandStatus::AlreadyOff);
  CHECK(r.command(ups::Command::Start) == ups::CommandStatus::Accepted);
  CHECK(r.command(ups::Command::Start) == ups::CommandStatus::Busy);
  CHECK(r.command(ups::Command::Stop) == ups::CommandStatus::Busy);
  CHECK(!r.controller.configure(config, r.now));
  CHECK(!r.controller.setSuspended(true, r.now));
  r.advance(50);
  r.online = true;
  r.advance(11);
  CHECK(r.status().result == ups::Result::Started);
  CHECK(r.command(ups::Command::Start) == ups::CommandStatus::AlreadyOn);
  CHECK(r.manual_starts == 1);
}

void manual_stop_suppresses_keep_on() {
  ups::Config config = fastConfig();
  config.auto_mode = ups::AutoMode::KeepOn;
  config.max_attempts = 3;
  Rig r(config, 0, true, true);
  r.advance(30);
  CHECK(r.command(ups::Command::Stop) == ups::CommandStatus::Accepted);
  r.advance(50);
  r.online = false;
  r.advance(31);
  CHECK(r.status().result == ups::Result::Stopped);
  CHECK(r.status().auto_hold);
  r.advance(1000);
  CHECK(r.automatic_starts == 0);
  CHECK(r.command(ups::Command::Resume) == ups::CommandStatus::Accepted);
  r.advance(99);
  CHECK(!r.controller.relayActive());
  r.advance(1);
  CHECK(r.automatic_starts == 1);
}

void stop_requires_present_on_feedback_not_a_blink_gap() {
  Rig r(fastConfig(), 0, true, true);
  r.advance(30);
  r.online = false;
  r.advance(11);
  CHECK(r.status().state == ups::State::Running);
  CHECK(!r.status().inputs_valid);
  CHECK(r.command(ups::Command::Stop) == ups::CommandStatus::InputsUnknown);
  CHECK(r.command(ups::Command::Restart) == ups::CommandStatus::InputsUnknown);
  CHECK(r.stop_presses == 0);
  r.online = true;
  r.advance(11);
  CHECK(r.command(ups::Command::Stop) == ups::CommandStatus::Accepted);
}

void failed_stop_is_reported_and_held() {
  ups::Config config = fastConfig();
  config.auto_mode = ups::AutoMode::KeepOn;
  Rig r(config, 0, true, true);
  r.advance(30);
  CHECK(r.command(ups::Command::Stop) == ups::CommandStatus::Accepted);
  r.advance(251);
  CHECK(r.status().result == ups::Result::StopFailed);
  CHECK(r.status().auto_hold);
  CHECK(!r.controller.relayActive());
  CHECK(r.stop_presses == 1);
}

void restart_does_not_start_after_unconfirmed_shutdown() {
  Rig r(fastConfig(), 0, true, true);
  r.advance(30);
  CHECK(r.command(ups::Command::Restart) == ups::CommandStatus::Accepted);
  r.advance(1000);
  CHECK(r.status().result == ups::Result::StopFailed);
  CHECK(r.stop_presses == 1);
  CHECK(r.manual_starts == 0);
  CHECK(r.automatic_starts == 0);
}

void restart_waits_for_confirmed_off_and_delay() {
  Rig r(fastConfig(), 0, true, true);
  r.advance(30);
  CHECK(r.command(ups::Command::Restart) == ups::CommandStatus::Accepted);
  r.advance(50);
  r.online = false;
  r.tick();
  r.advance(30);
  CHECK(r.status().phase == ups::Phase::RestartWait);
  CHECK(r.controller.busy());
  r.advance(49);
  CHECK(!r.controller.relayActive());
  r.advance(1);
  CHECK(r.controller.relayActive());
  CHECK(r.manual_starts == 1);
  r.online = true;
  r.advance(50);
  CHECK(!r.controller.relayActive());
  r.advance(11);
  CHECK(r.status().result == ups::Result::Started);
  CHECK(!r.status().auto_hold);
}

void restart_does_not_toggle_a_self_started_ups() {
  Rig r(fastConfig(), 0, true, true);
  r.advance(30);
  CHECK(r.command(ups::Command::Restart) == ups::CommandStatus::Accepted);
  r.advance(50);
  r.online = false;
  r.advance(31);
  CHECK(r.status().phase == ups::Phase::RestartWait);
  r.online = true;
  r.advance(60);
  CHECK(r.status().result == ups::Result::AlreadyOn);
  CHECK(r.manual_starts == 0);
}

void keep_on_retries_are_bounded_per_mains_cycle() {
  ups::Config config = fastConfig();
  config.auto_mode = ups::AutoMode::KeepOn;
  config.max_attempts = 3;
  config.verify_timeout_ms = 100;
  Rig r(config);
  r.advance(3000);
  CHECK(r.automatic_starts == 3);
  CHECK(r.status().attempts == 3);
  CHECK(r.status().result == ups::Result::StartFailed);
  CHECK(!r.controller.relayActive());
  r.mains = false;
  r.advance(11);
  r.mains = true;
  r.advance(111);
  CHECK(r.automatic_starts == 4);
  CHECK(r.status().attempts == 1);
}

void failed_manual_start_is_not_retried_automatically() {
  ups::Config config = fastConfig();
  config.auto_mode = ups::AutoMode::KeepOn;
  config.auto_on_boot = false;
  config.max_attempts = 3;
  Rig r(config);
  r.advance(30);
  CHECK(r.command(ups::Command::Start) == ups::CommandStatus::Accepted);
  r.advance(2000);
  CHECK(r.status().result == ups::Result::StartFailed);
  CHECK(r.status().auto_hold);
  CHECK(r.automatic_starts == 0);
  CHECK(r.manual_starts == 1);
}

void cancel_releases_and_holds_pending_automation() {
  Rig r;
  r.advance(110);
  CHECK(r.controller.relayActive());
  CHECK(r.command(ups::Command::Cancel) == ups::CommandStatus::Accepted);
  CHECK(!r.controller.relayActive());
  CHECK(!r.controller.busy());
  r.advance(1000);
  CHECK(r.status().result == ups::Result::Cancelled);
  CHECK(r.status().auto_hold);
  CHECK(r.automatic_starts == 1);
  Rig pending;
  pending.advance(30);
  CHECK(pending.command(ups::Command::Cancel) == ups::CommandStatus::Accepted);
  pending.advance(1000);
  CHECK(pending.automatic_starts == 0);
  Rig early;
  CHECK(early.command(ups::Command::Cancel) == ups::CommandStatus::Accepted);
  early.advance(1000);
  CHECK(early.automatic_starts == 0);
  CHECK(early.status().auto_hold);
}

void timers_survive_millis_rollover() {
  Rig r(fastConfig(), UINT32_MAX - 80);
  r.advance(109);
  CHECK(!r.controller.relayActive());
  CHECK(r.status().remaining_ms == 1);
  r.advance(1);
  CHECK(r.controller.relayActive());
  CHECK(r.status().remaining_ms == 50);
  r.advance(49);
  CHECK(r.controller.relayActive());
  r.advance(1);
  CHECK(!r.controller.relayActive());
  r.online = true;
  r.advance(11);
  CHECK(r.status().result == ups::Result::Started);
}

void restored_attempt_is_not_replayed_after_soft_reset() {
  Rig first;
  first.advance(361);
  const ups::Latch saved = first.controller.latch();
  CHECK(saved.attempts == 1);
  Rig restored(fastConfig(), 0, true, false, false, &saved);
  restored.advance(1000);
  CHECK(restored.automatic_starts == 0);
  CHECK(restored.status().cycle_handled);
  CHECK(restored.status().attempts == 1);
  restored.mains = false;
  restored.advance(11);
  restored.mains = true;
  restored.advance(111);
  CHECK(restored.automatic_starts == 1);
}

void restored_manual_hold_survives_and_new_mains_clears_it() {
  ups::Config config = fastConfig();
  config.auto_mode = ups::AutoMode::KeepOn;
  const ups::Latch saved = {true, true, true, true, 0};
  Rig restored(config, 0, true, false, false, &saved);
  restored.advance(1000);
  CHECK(restored.status().auto_hold);
  CHECK(restored.automatic_starts == 0);
  Rig after_loss(config, 0, false, false, false, &saved);
  after_loss.advance(30);
  CHECK(!after_loss.status().auto_hold);
  after_loss.mains = true;
  after_loss.advance(111);
  CHECK(after_loss.automatic_starts == 1);
}

void mains_return_during_manual_stop_keeps_reset_safe_hold() {
  Rig r(fastConfig(), 0, false, false, true);
  r.advance(30);
  CHECK(r.command(ups::Command::Stop) == ups::CommandStatus::Accepted);
  r.mains = true;
  r.advance(11);
  CHECK(r.status().phase == ups::Phase::PressStop);
  CHECK(r.status().auto_hold);
  CHECK(r.status().cycle_handled);
  const ups::Latch saved = r.controller.latch();
  Rig reset(fastConfig(), 0, true, false, false, &saved);
  reset.advance(1000);
  CHECK(reset.automatic_starts == 0);
}

void boot_autostart_can_be_disabled_until_a_new_cycle() {
  ups::Config config = fastConfig();
  config.auto_on_boot = false;
  config.auto_mode = ups::AutoMode::KeepOn;
  Rig r(config);
  r.advance(1000);
  CHECK(r.automatic_starts == 0);
  CHECK(r.status().auto_hold);
  r.mains = false;
  r.advance(11);
  r.mains = true;
  r.advance(111);
  CHECK(r.automatic_starts == 1);
}

void battery_start_requires_explicit_setting() {
  ups::Config config = fastConfig();
  config.auto_mode = ups::AutoMode::KeepOn;
  Rig r(config, 0, false);
  r.advance(30);
  CHECK(r.command(ups::Command::Start) == ups::CommandStatus::MainsRequired);
  config.allow_battery_start = true;
  CHECK(r.controller.configure(config, r.now));
  r.advance(31);
  CHECK(r.command(ups::Command::Start) == ups::CommandStatus::Accepted);
  r.advance(50);
  r.battery = true;
  r.advance(11);
  CHECK(r.status().result == ups::Result::Started);
  CHECK(r.automatic_starts == 0);
}

void configuration_preserves_hold_and_does_not_press() {
  Rig r;
  r.advance(30);
  CHECK(r.command(ups::Command::Cancel) == ups::CommandStatus::Accepted);
  ups::Config config = fastConfig();
  config.auto_mode = ups::AutoMode::KeepOn;
  config.start_delay_ms = 50;
  CHECK(r.controller.configure(config, r.now));
  CHECK(!r.controller.relayActive());
  r.advance(1000);
  CHECK(r.status().auto_hold);
  CHECK(r.automatic_starts == 0);
}

void suspension_prevents_automatic_actions_and_restores_delay() {
  Rig r;
  r.advance(40);
  CHECK(r.controller.setSuspended(true, r.now));
  r.advance(1000);
  CHECK(r.automatic_starts == 0);
  CHECK(r.command(ups::Command::Start) == ups::CommandStatus::Suspended);
  CHECK(r.controller.setSuspended(false, r.now));
  CHECK(!r.controller.relayActive());
  r.advance(99);
  CHECK(r.automatic_starts == 0);
  r.advance(1);
  CHECK(r.automatic_starts == 1);
}

int main() {
  one_attempt_per_mains_cycle();
  already_running_never_pressed();
  unstable_mains_does_not_start();
  blinking_indicators_do_not_look_off();
  on_transition_withdraws_old_off_decision();
  start_confirmation_is_measured_after_release();
  mains_loss_releases_an_active_press();
  commands_guard_state_and_concurrent_operations();
  manual_stop_suppresses_keep_on();
  stop_requires_present_on_feedback_not_a_blink_gap();
  failed_stop_is_reported_and_held();
  restart_does_not_start_after_unconfirmed_shutdown();
  restart_waits_for_confirmed_off_and_delay();
  restart_does_not_toggle_a_self_started_ups();
  keep_on_retries_are_bounded_per_mains_cycle();
  failed_manual_start_is_not_retried_automatically();
  cancel_releases_and_holds_pending_automation();
  timers_survive_millis_rollover();
  restored_attempt_is_not_replayed_after_soft_reset();
  restored_manual_hold_survives_and_new_mains_clears_it();
  mains_return_during_manual_stop_keeps_reset_safe_hold();
  boot_autostart_can_be_disabled_until_a_new_cycle();
  battery_start_requires_explicit_setting();
  configuration_preserves_hold_and_does_not_press();
  suspension_prevents_automatic_actions_and_restores_delay();
  std::cout << "25 controller behavior tests passed\n";
  return 0;
}
