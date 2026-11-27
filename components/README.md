# Local component overrides

## mqtt

A copy of ESPHome's `mqtt` component from **ESPHome 2026.9.1** (the version the Home Assistant
add-on runs), with two changes:

1. `MQTTClientComponent::check_connected()` no longer runs `delay(100)` after connecting. On
   battery candles, that delay was about 100 ms of a ~400 ms wake.
2. `MQTTClientComponent::outbox_size()` (ESP32 only) returns the QoS 1/2 publishes not yet
   acknowledged by the broker. `candleDev10.yaml` publishes its last messages with QoS 1 and
   waits for this to reach 0 before deep sleep; otherwise a dropped packet is never resent.

Only configs that declare it use it (`candleDev4.yaml` and `candleDev10.yaml`, via
`external_components`). Every other device uses the stock component.

**When ESPHome is upgraded**, re-copy the component from the new version and reapply the patches,
or remove the `external_components` entry. A stale copy can fail to build, or silently miss
upstream fixes. To find the patches, search `mqtt_client.cpp`, `mqtt_client.h` and `mqtt_backend_esp32.h` for `LOCAL PATCH`.

ESPHome's C++ code is GPLv3; these files keep their upstream licensing.
