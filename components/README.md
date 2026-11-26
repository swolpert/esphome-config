# Local component overrides

## mqtt

A copy of ESPHome's `mqtt` component from **ESPHome 2026.9.1** (the version the Home Assistant
add-on runs), with one change: `MQTTClientComponent::check_connected()` no longer runs
`delay(100)` after connecting. On battery candles, that delay was about 100 ms of a ~400 ms wake.

Only configs that declare it use it (`candleDev4.yaml`, via `external_components`). Every
other device uses the stock component.

**When ESPHome is upgraded**, re-copy the component from the new version and reapply the patch,
or remove the `external_components` entry. A stale copy can fail to build, or silently miss
upstream fixes. To find the patch, search `mqtt_client.cpp` for `LOCAL PATCH`.

ESPHome's C++ code is GPLv3; these files keep their upstream licensing.
