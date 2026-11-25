#pragma once

#include <stdint.h>

namespace ups {

enum class AutoMode : uint8_t { Disabled, OnMains, KeepOn };
enum class State : uint8_t { Unknown, Off, Online, Battery, Running };
enum class Phase : uint8_t {
  Idle, Waiting, Checking, PressStart, VerifyStart, PressStop, VerifyStop,
  RestartWait, Cooldown, Finished, Paused
};
enum class Result : uint8_t {
  None, AlreadyOn, Started, StartFailed, Stopped, StopFailed, Cancelled,
  MainsLost, AttemptsExhausted
};
enum class Command : uint8_t { Start, Stop, Restart, Cancel, Resume };
enum class CommandStatus : uint8_t {
  Accepted, Busy, InputsUnknown, AlreadyOn, AlreadyOff, MainsRequired, Suspended
};
enum class Event : uint8_t {
  Ready, MainsPresent, MainsLost, AlreadyOn, AutoStart, ManualStart,
  StopRequested, RelayReleased, StartConfirmed, StartFailed, StopConfirmed,
  StopFailed, RestartWaiting, Cancelled, AutoResumed, AttemptsExhausted,
  Restored, ConfigChanged, Suspended, Resumed
};

struct Config {
  AutoMode auto_mode;
  uint32_t start_delay_ms;
  uint32_t press_ms;
  uint32_t stop_press_ms;
  uint32_t verify_timeout_ms;
  uint32_t standby_filter_ms;
  uint32_t indicator_filter_ms;
  uint32_t off_confirm_ms;
  uint32_t restart_delay_ms;
  uint32_t retry_delay_ms;
  uint8_t max_attempts;
  bool auto_on_boot;
  bool allow_battery_start;

  Config();
};

// These fields are the semantic RTC payload. Serialize them explicitly rather
// than depending on the compiler's structure layout or padding.
struct Latch {
  bool cycle_known;
  bool mains_present;
  bool cycle_handled;
  bool auto_hold;
  uint8_t attempts;
};

struct Snapshot {
  State state;
  Phase phase;
  Result result;
  bool busy;
  bool relay;
  bool mains;
  bool online;
  bool battery;
  bool inputs_valid;
  bool auto_hold;
  bool cycle_handled;
  uint8_t attempts;
  uint32_t remaining_ms;
};

typedef void (*EventCallback)(void *context, Event event);

class Controller {
public:
  Controller();
  void setEventCallback(EventCallback callback, void *context);
  void begin(const Config &config, uint32_t now, const Latch *restored = 0);
  void tick(uint32_t now, bool raw_mains, bool raw_online, bool raw_battery);
  CommandStatus command(Command action, uint32_t now);
  bool configure(const Config &config, uint32_t now);
  bool setSuspended(bool suspended, uint32_t now);
  Snapshot snapshot(uint32_t now) const;
  Latch latch() const;
  bool busy() const;
  bool relayActive() const;
  const Config &config() const;

private:
  struct Filter {
    bool initialized;
    bool previous;
    bool value;
    bool valid;
    uint32_t changed_at;
    void reset();
    void update(bool raw, uint32_t now, uint32_t period);
  };

  Config config_;
  Latch latch_;
  Filter mains_filter_;
  Filter online_filter_;
  Filter battery_filter_;
  Phase phase_;
  Result result_;
  EventCallback callback_;
  void *callback_context_;
  uint32_t eligible_at_;
  uint32_t phase_at_;
  uint32_t retry_at_;
  uint32_t off_at_;
  bool first_mains_observation_;
  bool running_known_;
  bool running_;
  bool off_tracking_;
  bool relay_;
  bool automatic_action_;
  bool restarting_;
  bool suspended_;
  bool attempts_logged_;

  void emit(Event event);
  void resetIndicators();
  void updateIndicators(uint32_t now, bool raw_online, bool raw_battery);
  void observeMains(uint32_t now);
  void startPress(uint32_t now, bool automatic);
  void stopPress(uint32_t now, bool restart);
  void finish(Result result, Event event);
  void runAutomatic(uint32_t now);
  State state() const;
  bool startPowerAvailable() const;
};

const char *autoModeName(AutoMode mode);
const char *stateName(State state);
const char *phaseName(Phase phase);
const char *resultName(Result result);
const char *commandStatusName(CommandStatus status);
const char *eventName(Event event);

}  // namespace ups
