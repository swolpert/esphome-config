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

- **`candleDev4.yaml`** (used by `candle01`): **the active line.** The goal is a fast, reliable sleep→wake→sleep cycle. On each wake it waits for the light state on `candles/led` (an ESPHome light JSON payload, applied via `LightJSONSchema::parse_json`), the sleep interval on `candles/sleep_interval` (e.g. `10m`, `1h`, or `always on` to stay awake for OTA) and a battery ADC sample. Then `consider_deep_sleep` deep sleeps for that interval. An `interval:` of 12s re-runs the check.
- **`candleDev9.yaml`** (used by `candle-test`): an abandoned experiment with RTC alarm wakeups. It takes an `{"on_tod","off_tod"}` (`HH:MM:SS`) schedule on `ha/light/candles_rgb_mqtt/schedule`, switches the light itself, and programs DS3231 Alarm 1 (registers `0x07`–`0x0A`, control `0x0E`) for the next event, via raw `ds1307_time->read_bytes/write_bytes`. The DS3231 uses ESPHome's `ds1307` time platform. **It never decides to sleep**: `consider_deep_sleep` is commented out, so it only cycles the 5s fallback `deep_sleep`. The `doneNN_*` flags meant to gate sleep aren't read yet, and the ADC sets `done02_sntp` instead of `done03_adc`.

MQTT topics under `candles/` are shared by all candles; per-device topics use `${dev_name}`. Don't set a global `topic_prefix`.

JSON in lambdas: use `DynamicJsonDocument doc(1024);` and `obj["k"].is<T>()`. They compile on both ArduinoJson 6 and 7 (DynamicJsonDocument is deprecated on 7 but builds), so configs work on the user's pinned ESPHome and on newer ones. `JsonDocument doc;` is 7-only; `containsKey` is deprecated.

Deprecations due in ESPHome 2027.1: `web_server: version: 1` (used in the base layers), and `neopixelbus` on ESP32, which should move to `esp32_rmt_led_strip`.

## Wake time (battery candles, candleDev4)

The goal is a short wake → sleep cycle. The debug pin (GPIO10) goes high at boot and drops in an `esp_deep_sleep_register_hook()` callback, i.e. after ESPHome's shutdown/teardown, so a scope sees the whole wake. On 2026-11-25 the wake went from 5.7 s to ~0.41 s with a static IP. With DHCP (the current setup, chosen for easier IP management) it's ~1.4 s, because DHCP alone takes ~1.0 s on the IoT VLAN. To go back to a static IP, uncomment `manual_ip` in `baseCandle.yaml` and set `dev_static_ip` per candle. The user rejected caching the DHCP lease in RTC memory as too complex.

What mattered:
- **MQTT `enable_on_boot: false`, then `mqtt.enable` in `on_boot` once `wifi.connected`.** ESPHome starts MQTT at setup, before WiFi is up. That first attempt fails, and the client's 5 s retry throttle (`now - connect_begin_ > 5000`) pushed the connect to ~5.1 s after boot.
- **`components/mqtt`**: a vendored copy of ESPHome 2026.9.1's mqtt component without the hard-coded `delay(100)` after connect. See `components/README.md`; it must be refreshed or removed when ESPHome is upgraded.
- `mqtt: discovery: false`. The battery-voltage discovery config is already retained on the broker, and the lights are internal. Turn it on for one wake if entities are added.
- Logger at INFO in `baseCandle.yaml`, so there's no per-wake config dump or DEBUG output at 115200 baud.
- `esp32: framework: sdkconfig_options`: `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP` (~200 ms) and `CONFIG_BOOTLOADER_LOG_LEVEL_WARN` (~100 ms). **These live in the bootloader, and OTA never updates it.** Changes there need a USB flash of `firmware.factory.bin` (BOOT+RST into download mode, then web.esphome.io). `esp_deep_sleep_disable_rom_logging()` before sleep removes the ROM banner, and works over OTA. `CONFIG_LWIP_DHCP_RESTORE_LAST_IP` and `CONFIG_LWIP_DHCP_DOES_ARP_CHECK: n` are set but made no measurable difference.

Telemetry the candle publishes on each wake:
- `${dev_name}/waketime` (retained): `millis()` at association (`assoc`), DHCP (`gotip`), ESPHome WiFi connected, MQTT, first `candles/led`, first `candles/sleep_interval`, and the sleep decision, plus the current IP.
- `${dev_name}/bootlog`: the log lines from before MQTT connects (MQTT logging can't send them), excluding CONFIG-level lines.

Log lines logged right before sleeping often don't make it out over MQTT, so publish anything important directly. Serial logs come out on UART0 TX = GPIO21 at 115200 baud, not on the USB-C port.

`run_duration: 30s` is a deliberate safety net: if a wake gets stuck, the candle sleeps anyway so the battery survives (a frozen loop is caught by the task watchdog instead). `candles/sleep_interval` = `always on` prevents sleep as soon as it's received, overriding the 30 s fallback; it's meant for OTA.

**OTA procedure** (the user's rule): publish retained `always on` to `candles/sleep_interval`, wait until the candle stays reachable, upload from the ESPHome container (`esphome upload ... --device <ip from waketime>`), then publish the previous interval back (e.g. `8 sec (test)`). With DHCP the IP can change; read it from `candle01/waketime`.
