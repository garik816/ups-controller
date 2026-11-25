# ESP8266 HTTP API, v1

Local HTTP on port 80; same API on AP and router address. UI and API use HTTP Basic authentication (`admin`, default password `1234567890`). GET /api/status returns a per-boot `csrf_token`; every POST uses `X-UPS-Token`. No cross-origin access. JSON bodies only, except the firmware upload. Passwords are never returned by GET endpoints or logs.

## GET /api/status

```json
{"version":"1.0.0","csrf_token":"...","uptime_ms":10000,"free_heap":30000,"reset_reason":"...","storage_ok":true,"ups":{"state":"off","phase":"waiting","result":"none","busy":false,"relay":false,"mains":true,"online":false,"battery":false,"inputs_valid":true,"auto_hold":false,"cycle_handled":false,"attempts":0,"remaining_ms":1000},"network":{"mode":"standalone","ap_active":true,"ap_ssid":"UPS_wifi","ap_ip":"192.168.7.1","ap_clients":1,"sta_connected":false,"sta_ssid":"","sta_ip":"0.0.0.0","rssi":0,"fallback":false,"subnet_conflict":false}}
```

UPS state is `unknown`, `off`, `online`, `battery` or `running`; both indicators active is `running`. Phase and result are humanized in the UI; unmapped values must still be shown safely. Timestamps in logs use monotonic uptime, not a fictional clock.

## GET /api/config

```json
{"network":{"mode":"standalone","ssid":"","has_password":false,"ap_ssid":"UPS_wifi","ap_ip":"192.168.7.1","fallback_seconds":30},"ups":{"auto_mode":"on_mains","start_delay_ms":10000,"press_ms":2000,"stop_press_ms":2000,"verify_timeout_ms":15000,"standby_filter_ms":300,"indicator_filter_ms":300,"off_confirm_ms":2000,"restart_delay_ms":10000,"retry_delay_ms":60000,"max_attempts":1,"auto_on_boot":true,"allow_battery_start":false},"inputs":{"online_active_high":true,"battery_active_high":true,"standby_active_high":true},"security":{"username":"admin"}}
```

POST /api/config accepts partial objects matching GET, plus `network.password` (omit to preserve, explicit empty string for an open network), `security.password` (new password, omit to preserve). Accepted network modes: `standalone`, `router`, `both`. Auto modes: `disabled`, `on_mains`, `keep_on`. No configurable relay polarity/pin numbers: electrical changes belong in BoardConfig.h and require rebuilding. New network settings apply after the HTTP response; AP remains reachable while joining. Settings changes are rejected while the UPS is in an action. Saving settings never presses the power button.

Changing any input polarity is allowed only when the submitted `ups.auto_mode` is `disabled` (or already disabled). Inspect the displayed signals before enabling automation again. A save also retries router connection after a subnet conflict, even if the SSID is unchanged.

Validation limits: SSID 1..32 bytes in router/both, router password empty or 8..63 bytes; admin password 8..63 bytes. start_delay_ms 1000..600000; press_ms and stop_press_ms 100..10000; verify_timeout_ms 1000..120000 and > off_confirm_ms; standby_filter_ms and indicator_filter_ms 50..5000; off_confirm_ms 300..30000 and >= indicator_filter_ms; restart_delay_ms 1000..600000; retry_delay_ms 10000..3600000; max_attempts 1..3. fallback_seconds 10..120. AP name, IP and default AP password are fixed as requested; admin password is independently configurable.

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
