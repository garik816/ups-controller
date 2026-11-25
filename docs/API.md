# ESP8266 HTTP API, v1 (firmware 1.1.1)

Local HTTP on port 80; same API on AP and router address. UI and API use HTTP Basic authentication (`admin`, default password `1234567890`). GET /api/status returns a per-boot `csrf_token`; every POST uses `X-UPS-Token`. No cross-origin access. JSON bodies only, except the firmware upload. Passwords are never returned by GET endpoints or logs.

## GET /api/status

```json
{"version":"1.1.1","csrf_token":"...","uptime_ms":10000,"free_heap":30000,"reset_reason":"...","storage_ok":true,"ups":{"state":"off","phase":"waiting","result":"none","busy":false,"relay":false,"mains":true,"online":false,"battery":false,"inputs_valid":true,"auto_hold":false,"cycle_handled":false,"attempts":0,"remaining_ms":1000},"network":{"mode":"standalone","ap_active":true,"ap_ssid":"UPS_wifi","ap_ip":"192.168.7.1","ap_clients":1,"sta_connected":false,"sta_ssid":"","sta_ip":"0.0.0.0","rssi":0,"fallback":false,"subnet_conflict":false}}
```

UPS state is `unknown`, `off`, `online`, `battery` or `running`; both indicators active is `running`. Phase and result are humanized in the UI; unmapped values must still be shown safely. Timestamps in logs use monotonic uptime, not a fictional clock.

### Battery voltage telemetry

Firmware 1.1.0 adds a separate top-level `battery` object. `ups.battery` remains the digital BATTERY indicator; it is not a voltage reading.

```json
{"battery":{"enabled":true,"valid":true,"state":"ok","raw_adc":206,"voltage_mv":12606,"low":false,"saturated":false,"full_scale_mv":62600,"low_warning_mv":0}}
```

| State | Meaning |
|---|---|
| `disabled` | Optional measurement is off; no ADC sampling. |
| `warming_up` | Waiting for 16 fresh samples. |
| `ok` | Valid filtered voltage; display in volts, preferably one decimal. |
| `no_signal` | Average ADC code is 0 or 1; could be a disconnected input or near-zero voltage. It does not prove the battery is physically absent. |
| `overrange` | At least one ADC sample reached 1020 or higher; the voltage estimate is invalid. |
| `stale` | Reading is older than two seconds. |

`voltage_mv` is `null` unless `valid` is true. `raw_adc` is `null` in disabled/warming-up/stale states; otherwise it is the rounded average or the recent saturation sample. Voltage is calculated using the unrounded sum of 16 samples, collected no faster than every 50 ms. A new result normally arrives about every 0.8 s. A long sampling interruption discards the partial window.

`low` is notification-only: the value is true below the configured threshold and clears at threshold + 300 mV. Threshold zero disables the warning. Voltage does not start, stop or restart the UPS. There is no state-of-charge percentage or runtime estimate.

`full_scale_mv` is a mathematical ADC conversion coefficient, **not a safe battery-input limit**. The documented resistor circuits are for **0..16 V**, including a 12 V battery charged to 14.4 V. `overrange` only detects ADC clipping; it cannot protect incorrect wiring or detect every violation of that physical range.

## GET /api/config

```json
{"network":{"mode":"standalone","ssid":"","has_password":false,"ap_ssid":"UPS_wifi","ap_ip":"192.168.7.1","fallback_seconds":30},"ups":{"auto_mode":"on_mains","start_delay_ms":10000,"press_ms":2000,"stop_press_ms":2000,"verify_timeout_ms":15000,"standby_filter_ms":300,"indicator_filter_ms":300,"off_confirm_ms":2000,"restart_delay_ms":10000,"retry_delay_ms":60000,"max_attempts":1,"auto_on_boot":true,"allow_battery_start":false},"inputs":{"online_active_high":true,"battery_active_high":true,"standby_active_high":true},"security":{"username":"admin"}}
```

POST /api/config accepts partial objects matching GET, plus `network.password` (omit to preserve, explicit empty string for an open network), `security.password` (new password, omit to preserve). Accepted network modes: `standalone`, `router`, `both`. Auto modes: `disabled`, `on_mains`, `keep_on`. No configurable relay polarity/pin numbers: electrical changes belong in BoardConfig.h and require rebuilding. New network settings apply after the HTTP response; AP remains reachable while joining. Settings changes are rejected while the UPS is in an action. Saving settings never presses the power button.

Changing any input polarity is allowed only when the submitted `ups.auto_mode` is `disabled` (or already disabled). Inspect the displayed signals before enabling automation again. A save also retries router connection after a subnet conflict, even if the SSID is unchanged.

Validation limits: SSID 1..32 bytes in router/both, router password empty or 8..63 bytes; admin password 8..63 bytes. start_delay_ms 1000..600000; press_ms and stop_press_ms 100..10000; verify_timeout_ms 1000..120000 and > off_confirm_ms; standby_filter_ms and indicator_filter_ms 50..5000; off_confirm_ms 300..30000 and >= indicator_filter_ms; restart_delay_ms 1000..600000; retry_delay_ms 10000..3600000; max_attempts 1..3. fallback_seconds 10..120. AP name, IP and default AP password are fixed as requested; admin password is independently configurable.

Firmware 1.1.0 also returns and accepts:

```json
{"battery":{"enabled":false,"full_scale_mv":62600,"low_warning_mv":0}}
```

`enabled` must be boolean; `full_scale_mv` must be an integer in 10000..100000; `low_warning_mv` is integer zero (off) or 8000..14000. Defaults are off / 62600 / 0. The nominal scale 62600 matches the external 180k/10k divider with the NodeMCU onboard 220k/100k divider; 19000 matches the same external resistors connected to a bare 1 V ADC. External 220k/10k uses 75800 with the same NodeMCU onboard divider, or 23000 with a bare 1 V ADC. Choose the actual hardware and calibrate against a multimeter. Firmware 1.1.1 raises the coefficient limit from 80000 to 100000 to accommodate resistor tolerances; the physical battery range remains 0..16 V.

Changing only voltage/network/security settings does not reset UPS filters or autostart timers. Firmware accepts complete stored schema-1 settings from 1.0.x and preserves their UPS/network/password settings with voltage measurement disabled. New saves use schema 2 and require all three battery keys in a stored snapshot. HTTP patches remain partial.

## POST /api/battery/calibrate

```json
{"reference_mv":12600}
```

Enter a simultaneous multimeter reading at a stable battery voltage. Measurement must already be enabled with a valid fresh sample. UPS actions, firmware upload, network changes, scans and pending reboot block calibration with 409. Normal Basic authentication and `X-UPS-Token` are required.

The server accepts integer `reference_mv` 8000..16000 and ADC codes 10..1019, computes `round(reference_mv * 1023 / raw_adc)`, and accepts a resulting `full_scale_mv` in 10000..100000. Invalid inputs return 400. The new coefficient is saved atomically; a storage failure returns 500 and leaves the running coefficient unchanged. Success restarts only ADC averaging, not UPS control, and returns the normal success object. Read GET /api/config to retrieve the saved coefficient.

## POST /api/action

```json
{"action":"start"}
```

Actions: `start`, `stop`, `restart`, `cancel`, `resume`. UI requires explicit confirmation for stop/restart. Manual start/stop use confirmed indicator state; firmware refuses inappropriate commands and concurrent actions. Stop suppresses automatic restart for this mains cycle. Resume explicitly clears that suppression and re-evaluates configured automation. Cancel immediately releases relay and cancels pending actions; it does not power-cycle a running UPS. UI displays server error text and never assumes a command succeeded.

## GET /api/logs

```json
{"events":[{"id":1,"uptime_ms":2000,"level":"info","message":"..."}]}
```

## POST /api/scan and GET /api/scan

Start asynchronous Wi-Fi scan, then poll GET. Results: `{"running":false,"networks":[{"ssid":"home","rssi":-55,"secure":true}]}`. UI must use textContent for network names. Scan/action/config/firmware conflicts return 409 rather than delaying a relay pulse.

## POST /api/reboot

Reboots only the ESP controller; rejected during UPS actions. Soft reset retains the current mains-cycle suppression in RTC memory. No button press is produced merely by rebooting.

## POST /api/update

Multipart upload field `firmware`, ESP8266 `.bin` only. Basic authentication and CSRF required. Reject if an action is busy. Automatic control is suspended during upload, and controller reboots only after a successful firmware update. Upload failure resumes the prior automation state without a power-button pulse.

## Responses

Success: `{"ok":true,"message":"..."}`. Failure: `{"ok":false,"error":"..."}` with 400 validation, 401 authentication, 403 token, 409 action conflict, 413 size, or 500 storage/update error. All API responses are `Cache-Control: no-store`.
