# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

The user's ESPHome device configs for their Home Assistant setup, mirrored from HA's `esphome/` config folder. **This repo is public** (github.com/swolpert/esphome-config), so never commit literal passwords, keys or hashes; use `!secret`. `secrets.yaml` and `.esphome/` are gitignored and not present here.

**The user's Home Assistant and ESPHome are deliberately not up to date** (some upgrades broke things), and they are staying on their current version for now. Configs must build on *their* ESPHome version, not only the latest. Check which version they run before relying on newer syntax, and don't suggest upgrading as a fix.

There is no test suite. The check for a change is `esphome config <device>.yaml` (schema validation) followed by `esphome compile <device>.yaml`. Only compile catches errors inside C++ `lambda:` blocks, which is where ESPHome and ArduinoJson upgrades usually break things. Build on the user's existing Home Assistant ESPHome stack rather than installing ESPHome locally: a first local compile pulls about 2.8 GB of ESP-IDF toolchain and is very slow under WSL. Always point ESPHome at a top-level device file; the dot-files and `candleDevN.yaml` are fragments and fail on their own.

## Layering

Every device file sets `substitutions:` (`dev_name`, `dev_friendly_name`, `dev_use_address`, sometimes `dev_static_ip`) and merges shared layers with `<<: !include ...`. Files starting with `.` are shared layers, and several are old versions kept for reference.

| Devices | Base layer (wifi/ota/logger/web_server) | Device layer |
|---|---|---|
| `candle02`–`candle10` | `.base.yaml` | `.esp32_candlestick.yaml`: always-on, Home Assistant time |
| `candle01`, `candle-test` | `baseCandle.yaml` (static IP, MQTT-era) | `candleDevN.yaml`: battery/deep-sleep development versions |
| `den1`, `piano1`, `plug01`, `lamp-plug`, `play1`, `dining1`, `lemon-tree` | `.base.yaml` | `.etekcity_plug.yaml` (ESP8266 `esp01_1m`) |
| `den-fanlight` | `.base-tuya.yaml` | inline |
| `guest-fanlight`, `studio-fanlight`, `guest-2gang-switch`, `kids-lamp-rgb`, `tubeszb-*` | self-contained | |

Put per-device values in the device file's substitutions, not in shared layers. A change to a shared layer affects every device that includes it.

`candle01`, `candle02`, `candle-test` and `den1` were checked against ESPHome 2026.11.1. All four validate, and the three candles also compile. Current ESPHome rejects `update_interval` on `wifi_info` sub-sensors; the old dot-file variants still have it.

## Candle hardware

DFRobot Beetle ESP32-C3 (Arduino framework). SK6812 RGBW LED on GPIO0 (`neopixelbus`, GRBW). Blue status LED on GPIO8 (LEDC PWM, inverted; strapping pin). Battery ADC on GPIO3. `debug_gpio` on GPIO10 goes high at boot so wake time can be measured on a scope. Some boards have a DS3231 RTC on I2C (SDA GPIO4 / SCL GPIO5, address 0x68).

## candleDev versions (battery candles, MQTT-controlled)

`candleDevN.yaml` (and the `.esp32_candlestick_dev*.yaml` / `.candle_mqtt_schedule_dev*` dot-files) were the user's version control before they used git. Keep them as a record of progress, but don't edit or "fix" old numbers. File dates don't follow the numbering. The line went 1→9, the RTC-alarm approach in 9 didn't work out, and **development continued from 4**. All of these versions talk MQTT only (broker 192.168.1.200, no `api:`).

- **`candleDev4.yaml`**: the line that `candleDev10` grew from (fast wake work, see below). Its `${dev_name}/waketime` publish is broken: `publish(topic, char*, 0, true)` resolves to the `(topic, payload, length, qos)` overload and sends an *empty* retained message, which wipes the topic. Dev10 passes `std::string`s.
  Original description: The goal is a fast, reliable sleep→wake→sleep cycle. On each wake it waits for the light state on `candles/led` (an ESPHome light JSON payload, applied via `LightJSONSchema::parse_json`), the sleep interval on `candles/sleep_interval` (e.g. `10m`, `1h`, or `always on` to stay awake for OTA) and a battery ADC sample. Then `consider_deep_sleep` deep sleeps for that interval. An `interval:` of 12s re-runs the check.
- **`candleDev10.yaml`** (used by `candle01`): **the active line.** Dev4 plus a mirrored schedule and schedule-aware sleeps. C++ is in `candle_schedule.h`.
  - HA publishes a retained rule to `candles/schedule`: `{"en":1,"on":"sunset-30","off":"22:00"}`. `on`/`off` are `HH:MM` or `sunset±N`/`sunrise±N` (minutes, the same syntax as Simple Scheduler). The candle computes the times with ESPHome's `sun` component (`!secret candle_latitude`/`candle_longitude`) and keeps the rule in RTC memory (lost on a battery swap).
  - **HA decides**: the retained `candles/led` is applied on every wake that reaches MQTT and overrides the schedule. The candle switches by itself only on EVENT wakes and when WiFi/MQTT is down (`offline_sleep`).
  - `candles/sleep_interval` now means the max time between check-ins (`12 hr` normally; `always on` for OTA).
  - Wake kinds (in `waketime` as `kind=`): `check` (routine), `pre` (a long ESP-timer sleep aimed ~3% + 60 s early, WiFi + SNTP resync), `event` (precise short sleep landing 2 s after the event: no WiFi, set the LED, sleep). Without a valid clock it sleeps at most 15 min.
  - Time: SNTP to IPs only (the IoT VLAN firewall allows NTP only to a fixed list: 162.159.200.123/.1, 216.239.35.0, 129.6.15.28-30, 168.61.215.74, 132.163.96.5 (times out)). ESP-IDF system time keeps running through deep sleep. `esp_sntp_restart()` after WiFi connects, otherwise the first sync takes ~30 s (ESPHome starts SNTP before WiFi and lwIP backs off).
  - HA side: `ha/candles_package.yaml` (installed as `/homeassistant/packages/candles.yaml`): dashboard helpers (`input_select.candles_on_mode`, `input_number.candles_on_offset`, `input_datetime.candles_on_time`/`candles_off_time`, `input_boolean.candles_schedule`), the rule sensor, `sensor.candles_on_today`, and automations that publish the rule and switch `light.candles_rgb_mqtt`. It replaced the Simple Scheduler + Node-RED "SimpleSchedule to MQTT" pipeline.
- **`candleDev9.yaml`** (used by `candle-test`): an abandoned experiment with RTC alarm wakeups. It takes an `{"on_tod","off_tod"}` (`HH:MM:SS`) schedule on `ha/light/candles_rgb_mqtt/schedule`, switches the light itself, and programs DS3231 Alarm 1 (registers `0x07`–`0x0A`, control `0x0E`) for the next event, via raw `ds1307_time->read_bytes/write_bytes`. The DS3231 uses ESPHome's `ds1307` time platform. **It never decides to sleep**: `consider_deep_sleep` is commented out, so it only cycles the 5s fallback `deep_sleep`. The `doneNN_*` flags meant to gate sleep aren't read yet, and the ADC sets `done02_sntp` instead of `done03_adc`.

MQTT topics under `candles/` are shared by all candles; per-device topics use `${dev_name}`. Don't set a global `topic_prefix`.

JSON in lambdas: use `DynamicJsonDocument doc(1024);` and `obj["k"].is<T>()`. They compile on both ArduinoJson 6 and 7 (DynamicJsonDocument is deprecated on 7 but builds), so configs work on the user's pinned ESPHome and on newer ones. `JsonDocument doc;` is 7-only; `containsKey` is deprecated.

Deprecations due in ESPHome 2027.1: `web_server: version: 1` (used in the base layers), and `neopixelbus` on ESP32, which should move to `esp32_rmt_led_strip`.

## Wake time (battery candles, candleDev4 and 10)

The goal is a short wake → sleep cycle. The debug pin (GPIO10) goes high at boot and drops in an `esp_deep_sleep_register_hook()` callback, i.e. after ESPHome's shutdown/teardown, so a scope sees the whole wake. On 2026-11-25 the wake went from 5.7 s to ~0.41 s with a static IP. With DHCP (the current setup, chosen for easier IP management) it's ~1.4 s, because DHCP alone takes ~1.0 s on the IoT VLAN. To go back to a static IP, uncomment `manual_ip` in `baseCandle.yaml` and set `dev_static_ip` per candle. The user rejected caching the DHCP lease in RTC memory as too complex.

What mattered:
- **MQTT `enable_on_boot: false`, then `mqtt.enable` in `on_boot` once `wifi.connected`.** ESPHome starts MQTT at setup, before WiFi is up. That first attempt fails, and the client's 5 s retry throttle (`now - connect_begin_ > 5000`) pushed the connect to ~5.1 s after boot.
- **`components/mqtt`**: a vendored copy of ESPHome 2026.9.1's mqtt component without the hard-coded `delay(100)` after connect. See `components/README.md`; it must be refreshed or removed when ESPHome is upgraded.
- `mqtt: discovery: false`. The battery-voltage discovery config is already retained on the broker, and the lights are internal. Turn it on for one wake if entities are added.
- Logger at INFO in `baseCandle.yaml`, so there's no per-wake config dump or DEBUG output at 115200 baud.
- `esp32: framework: sdkconfig_options`: `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP` (~200 ms) and `CONFIG_BOOTLOADER_LOG_LEVEL_WARN` (~100 ms). **These live in the bootloader, and OTA never updates it.** Changes there need a USB flash of `firmware.factory.bin` (BOOT+RST into download mode, then web.esphome.io). `esp_deep_sleep_disable_rom_logging()` before sleep removes the ROM banner, and works over OTA. `CONFIG_LWIP_DHCP_RESTORE_LAST_IP` and `CONFIG_LWIP_DHCP_DOES_ARP_CHECK: n` are set but made no measurable difference.

Telemetry the candle publishes on each wake:
- `${dev_name}/waketime` (retained): `millis()` at association (`assoc`), DHCP (`gotip`), ESPHome WiFi connected, MQTT, first `candles/led`, first `candles/sleep_interval`, and the sleep decision, plus the current IP.
- `${dev_name}/wake` (Dev10, not retained): sent the moment MQTT connects: wake kind, `esp_reset_reason()`, RSSI, WiFi channel, the assoc/DHCP/WiFi/MQTT timings and battery voltage.
- Retained-message latency: Mosquitto (Nagle on) sends the first SUBACK, then holds the retained `candles/*` messages until the candle ACKs it, and lwIP sends delayed ACKs only on its TCP timer tick (`CONFIG_LWIP_TCP_TMR_INTERVAL`, default 250 ms). That cost ~165 ms per wake (the old 1 KB bootlog publish happened to carry the ACK early). Dev10 sets the interval to 50 ms.
- `${dev_name}/bootlog` (Dev4 only; removed in Dev10 to save time and memory on each wake, use the serial log instead): the log lines from before MQTT connects.

Log lines logged right before sleeping often don't make it out over MQTT, so publish anything important directly. Even direct publishes got lost, for two reasons found on 2026-11-26: (1) with ESPHome's default `clean_session: false`, Mosquitto kept the candle's session across sleeps, including the QoS 2 `esphome/discover` replies that deep sleep cut off; they filled the in-flight quota and Mosquitto then rejected every QoS 1/2 publish ("exceeded its receive-maximum quota" on every connect, since at least Dev4). Dev10 sets `clean_session: true`. (2) About 1 in 6 wakes loses a packet on the IoT WiFi (WiFi power save off didn't help). Dev10 sends its last publishes with QoS 1 and waits up to 3 s for the broker's ack via the patched `outbox_size()`; TCP's retransmit recovers them at ~1.5 s. `waketime` carries `n` (wake counter), `prev_pub` and `prev_wait_ms` for the previous wake's publish. Serial logs come out on UART0 TX = GPIO21 at 115200 baud, not on the USB-C port.

`run_duration: 30s` is a deliberate safety net: if a wake gets stuck, the candle sleeps anyway so the battery survives (a frozen loop is caught by the task watchdog instead). `candles/sleep_interval` = `always on` prevents sleep as soon as it's received, overriding the 30 s fallback; it's meant for OTA.

**OTA procedure** (candleDev10+), for any number of candles: in the ESPHome Device Builder click **Install** on each candle while it's asleep. The builder runs a compile-only job and **queues** the upload ("Device … is offline. Queuing compile-only job"); it builds in `/data/build/<name>` inside the add-on, not `/config/esphome/.esphome`. Then press HA's **"Candles OTA window"** button (Haus dashboard, Candle schedule card) once. It publishes a retained `candles/ota_request` `{"id","until","window_s":900}`, valid 13 h. At its next WiFi wake (up to the sleep interval later) each candle serves the request id once (kept in flash, so not again after the OTA reboot): it stays awake up to `window_s` and publishes `<name>/ota_ready`. The builder sees the MQTT `status: online` ("woke up. Triggering queued offline update") and uploads; without the window the upload races the ~1.5 s wake. HA shows a persistent notification per open window; it clears when the candle clears `ota_ready` (window over, or first sleep after the reboot). Firmware without the window (Dev4, early Dev10) needs the old way: retained `always on` on `candles/sleep_interval`, upload while it's awake, restore the interval.
