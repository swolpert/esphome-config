#pragma once
// C++ for the battery candles (candle/*.yaml). The YAML calls these functions; the logic lives here.
//
// Part 1: schedule and sleep planning (RTC memory, survives deep sleep).
//   HA publishes a retained rule on candles/schedule, e.g. {"en":1,"on":"sunset-30","off":"22:00"}.
//   The candle keeps the parsed rule in RTC memory (lost on a battery swap), so it can switch on/off
//   at the scheduled times by itself. HA's retained candles/led still overrides it on every wake that
//   reaches MQTT.
//   Wake kinds:
//     CHECK     - routine check-in (WiFi + SNTP + MQTT), at most `cap` apart
//     PRE_EVENT - timer wake aimed a little before an event, to resync the clock (WiFi + SNTP + MQTT)
//     EVENT     - precise timer wake at the event: apply the scheduled state, no WiFi, sleep again
//
// Part 2: the wake itself (RAM, reset every boot): milestones, MQTT inputs, OTA window, telemetry,
//   and going to sleep.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/time.h>
#include <esp_attr.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <driver/gpio.h>
#include "esphome/core/defines.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/components/deep_sleep/deep_sleep_component.h"
#include "esphome/components/light/light_json_schema.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/mqtt/mqtt_client.h"
#include "esphome/components/sun/sun.h"
#include "esphome/components/time/posix_tz.h"

namespace candle {

// ===================================================================================================
// Part 1: schedule and sleep planning
// ===================================================================================================

enum WakeKind : uint8_t { WAKE_CHECK = 0, WAKE_PRE_EVENT = 1, WAKE_EVENT = 2 };
static const char *const WAKE_NAMES[] = {"check", "pre", "event"};

enum RuleKind : uint8_t { RULE_NONE = 0, RULE_TIME = 1, RULE_SUNSET = 2, RULE_SUNRISE = 3 };
struct Rule {
  uint8_t kind;
  int16_t minutes;  // RULE_TIME: minutes after local midnight; sun rules: offset in minutes
};

static constexpr uint32_t RTC_MAGIC = 0xCA9D1E11;  // bump when RtcState changes
static constexpr double SUN_ELEVATION = -0.83333;  // standard sunrise/sunset, same as HA

struct RtcState {
  uint32_t magic;
  uint8_t en;              // schedule enabled
  Rule on, off;
  uint8_t next_kind;       // WakeKind planned for the next wake
  uint8_t event_state;     // state to apply on an EVENT wake (1 = on)
  int64_t event_target;    // scheduled epoch of the pending event
  int64_t last_event_target, last_event_actual;  // last EVENT wake, reported on the next WiFi wake
  int64_t cap_s;           // max seconds between check-ins (candles/sleep_interval), 0 = unknown
  // Sleep-timer calibration. The RTC slow clock (internal RC) runs a few % off; deep sleep and the
  // system clock during sleep both use it. Requested sleep = planned * sleep_scale.
  double sleep_scale;      // 0 = not calibrated yet (treated as 1.0)
  int64_t last_planned_ms, last_requested_ms;  // the sleep that ended with this wake
  // Diagnostics for the end-of-wake publish, reported in the next waketime.
  uint32_t wake_count;
  uint8_t prev_pub;        // 0 = not connected, 1 = publish failed, 2 = queued, 3 = acked
  uint16_t prev_wait_ms;   // time waited for the ack
  // The scheduled turn-on look (candles/schedule "look": [r, g, b, w, brightness], 0-255 each).
  uint8_t look_valid;
  uint8_t look[5];
};
static RTC_DATA_ATTR RtcState rtc;

static void rtc_init_if_needed() {
  if (rtc.magic != RTC_MAGIC) {
    memset(&rtc, 0, sizeof(rtc));
    rtc.magic = RTC_MAGIC;
  }
  if (!(rtc.sleep_scale > 0.9 && rtc.sleep_scale < 1.1)) rtc.sleep_scale = 1.0;
}

// Kind of the current wake, decided once at boot. Only a timer wake can be an EVENT/PRE_EVENT wake.
static uint8_t boot_kind = WAKE_CHECK;
static void decide_boot_kind() {
  rtc_init_if_needed();
  rtc.wake_count++;
  bool timer = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
  boot_kind = timer ? rtc.next_kind : (uint8_t) WAKE_CHECK;
  if (boot_kind > WAKE_EVENT) boot_kind = WAKE_CHECK;
  if (!timer) rtc.last_planned_ms = rtc.last_requested_ms = 0;  // reset/power-on: no sleep to account for
  // Until a new plan is made, a wake (e.g. after the run_duration fallback sleep) is a CHECK.
  rtc.next_kind = WAKE_CHECK;
}

static bool time_valid(time_t t) { return t > 1700000000; }  // after Nov 2023

// The system clock advanced by the *requested* sleep (as counted by the RTC clock), but with a
// calibrated scale the real sleep was the *planned* one: step the clock back by the difference.
// Call once, early on a timer wake.
static void correct_clock_after_sleep() {
  int64_t diff_ms = rtc.last_planned_ms - rtc.last_requested_ms;
  if (rtc.last_requested_ms <= 0 || diff_ms == 0) return;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (!time_valid(tv.tv_sec)) return;
  int64_t us = (int64_t) tv.tv_sec * 1000000 + tv.tv_usec + diff_ms * 1000;
  tv.tv_sec = (time_t) (us / 1000000);
  tv.tv_usec = (suseconds_t) (us % 1000000);
  settimeofday(&tv, nullptr);
}

// After a real SNTP sync on a timer wake: err_ms = SNTP time - (corrected) system time. The real
// sleep was planned + err; the RTC counted `requested` for it. Update the scale (smoothed).
// Returns the new estimate in ppm (0 if not updated).
static int32_t calibrate(int64_t err_ms) {
  if (rtc.last_planned_ms < 120000 || rtc.last_requested_ms <= 0) return 0;  // too short to measure
  double real_ms = (double) (rtc.last_planned_ms + err_ms);
  if (real_ms <= 0) return 0;
  double fresh = (double) rtc.last_requested_ms / real_ms;  // RTC counts per real ms
  if (!(fresh > 0.9 && fresh < 1.1)) return 0;
  // Weight by sleep length: a long sleep measures the rate much better than a short one.
  double w = rtc.last_planned_ms >= 3600000 ? 0.7 : 0.4;
  rtc.sleep_scale = rtc.sleep_scale * (1.0 - w) + fresh * w;
  return (int32_t) ((fresh - 1.0) * 1e6);
}

// Record a sleep about to start; returns the duration to request from the timer (ms).
static uint32_t begin_sleep_ms(int64_t planned_ms) {
  int64_t req = (int64_t) (planned_ms * rtc.sleep_scale);
  if (req < 1000) req = 1000;
  rtc.last_planned_ms = planned_ms;
  rtc.last_requested_ms = req;
  return (uint32_t) req;
}

// "HH:MM", "HH:MM:SS", "sunset", "sunset-30", "sunrise+15"
static bool parse_rule(const char *s, Rule &out) {
  if (s == nullptr) return false;
  while (*s == ' ') s++;
  uint8_t kind = RULE_NONE;
  if (strncasecmp(s, "sunset", 6) == 0) kind = RULE_SUNSET;
  else if (strncasecmp(s, "sunrise", 7) == 0) kind = RULE_SUNRISE;
  if (kind != RULE_NONE) {
    s += (kind == RULE_SUNSET) ? 6 : 7;
    while (*s == ' ') s++;
    int off = 0;
    if (*s == '+' || *s == '-') {
      if (s[1] < '0' || s[1] > '9') return false;
      char *end;
      long v = strtol(s, &end, 10);
      if (*end != '\0' && *end != ' ') return false;
      off = (int) v;
    } else if (*s != '\0') {
      return false;
    }
    if (off < -720 || off > 720) return false;
    out.kind = kind;
    out.minutes = (int16_t) off;
    return true;
  }
  int h, m;
  if (sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return false;
  out.kind = RULE_TIME;
  out.minutes = (int16_t) (h * 60 + m);
  return true;
}

// Local UTC offset (seconds to add to UTC) at instant t, from ESPHome's pre-parsed timezone.
static int32_t local_offset(time_t t) {
  const auto &tz = esphome::time::get_global_tz();
  bool dst = tz.has_dst() && esphome::time::is_in_dst(t, tz);
  return -(dst ? tz.dst_offset_seconds : tz.std_offset_seconds);
}

// First occurrence of the rule strictly after t (0 if none, e.g. no sunset).
static time_t next_after(const Rule &r, time_t t, esphome::sun::Sun *sun) {
  if (r.kind == RULE_TIME) {
    int64_t wall = (int64_t) t + local_offset(t);
    int64_t midnight = wall - (((wall % 86400) + 86400) % 86400);
    for (int d = 0; d < 3; d++) {
      int64_t target_wall = midnight + d * 86400LL + r.minutes * 60LL;
      // Convert local wall time to UTC; re-evaluate the offset at the candidate (DST changes).
      time_t guess = (time_t) (target_wall - local_offset(t));
      time_t utc = (time_t) (target_wall - local_offset(guess));
      if (utc > t) return utc;
    }
    return 0;
  }
  if ((r.kind == RULE_SUNSET || r.kind == RULE_SUNRISE) && sun != nullptr) {
    time_t base = t - r.minutes * 60 + 1;  // event + offset > t  <=>  event > t - offset
    auto date = esphome::ESPTime::from_epoch_utc(base);
    auto ev = (r.kind == RULE_SUNSET) ? sun->sunset(date, SUN_ELEVATION) : sun->sunrise(date, SUN_ELEVATION);
    if (!ev.has_value()) return 0;
    return ev->timestamp + r.minutes * 60;
  }
  return 0;
}

// Latest occurrence of the rule at or before t (0 if none).
static time_t prev_at_or_before(const Rule &r, time_t t, esphome::sun::Sun *sun) {
  time_t e = next_after(r, t - 2 * 86400, sun);
  if (e == 0 || e > t) return 0;
  for (int i = 0; i < 4; i++) {
    time_t n = next_after(r, e, sun);
    if (n == 0 || n > t) break;
    e = n;
  }
  return e;
}

static bool schedule_known() { return rtc.en && rtc.on.kind != RULE_NONE && rtc.off.kind != RULE_NONE; }

// Scheduled light state at time t: on if the latest past event was an "on".
static bool scheduled_state(time_t t, esphome::sun::Sun *sun) {
  time_t last_on = prev_at_or_before(rtc.on, t, sun);
  time_t last_off = prev_at_or_before(rtc.off, t, sun);
  return last_on > last_off;
}

// Next scheduled event after t; returns 0 if none. `state` gets the state it switches to.
static time_t next_event(time_t t, esphome::sun::Sun *sun, bool &state) {
  time_t n_on = next_after(rtc.on, t, sun);
  time_t n_off = next_after(rtc.off, t, sun);
  if (n_on == 0 && n_off == 0) return 0;
  if (n_off == 0 || (n_on != 0 && n_on < n_off)) {
    state = true;
    return n_on;
  }
  state = false;
  return n_off;
}

// Margins for the two-stage wake. Tune from the drift_ms telemetry.
static constexpr int64_t PRECISE_WINDOW_S = 300;  // within this, sleep straight to the event
static constexpr double DRIFT_MARGIN = 0.03;      // wake this fraction early on long sleeps
static constexpr int64_t EARLY_EXTRA_S = 60;      // plus this many seconds
static constexpr int64_t EVENT_LATE_S = 2;        // land this long after the event
static constexpr int64_t NO_TIME_RETRY_S = 900;   // max sleep while the clock is not set

// Plan the next sleep. Sets rtc.next_kind / event_* and returns the sleep length in seconds.
// cap_s: max time between check-ins (candles/sleep_interval); 0 or less means unknown.
static uint32_t plan_sleep(time_t now, int64_t cap_s, esphome::sun::Sun *sun, int64_t &next_ev_out) {
  if (cap_s <= 0) cap_s = 3600;  // no interval known (e.g. offline after a battery swap): 1 h
  next_ev_out = 0;
  rtc.next_kind = WAKE_CHECK;
  if (schedule_known() && time_valid(now)) {
    bool st = false;
    time_t ev = next_event(now, sun, st);
    if (ev != 0) {
      next_ev_out = ev;
      int64_t r = (int64_t) ev - now;
      if (r <= PRECISE_WINDOW_S) {
        rtc.next_kind = WAKE_EVENT;
        rtc.event_state = st ? 1 : 0;
        rtc.event_target = ev;
        return (uint32_t) (r + EVENT_LATE_S);
      }
      if (r <= cap_s) {
        int64_t s = (int64_t) (r * (1.0 - DRIFT_MARGIN)) - EARLY_EXTRA_S;
        if (s > r - PRECISE_WINDOW_S / 2) s = r - PRECISE_WINDOW_S / 2;  // land mid-way in the precise window
        if (s < 10) s = 10;
        rtc.next_kind = WAKE_PRE_EVENT;
        return (uint32_t) s;
      }
    }
  }
  // Without a valid clock the schedule can't be followed: retry the time sync soon.
  if (!time_valid(now) && cap_s > NO_TIME_RETRY_S) return (uint32_t) NO_TIME_RETRY_S;
  return (uint32_t) cap_s;
}

// The light's color/brightness/effect survive deep sleep through ESPHome's preferences (synced at
// shutdown), so switching off, or on without a look, only has to change the state.
// look: switch on with the scheduled look (same mapping as HA's JSON light: rgb carries the color
// brightness, w the white channel, brightness scales both).
static void apply_state(esphome::light::LightState *ls, bool on, bool look = false) {
  auto call = ls->make_call();
  call.set_state(on);
  if (on && look && rtc.look_valid) {
    const uint8_t *v = rtc.look;
    call.set_red(v[0] / 255.0f);
    call.set_green(v[1] / 255.0f);
    call.set_blue(v[2] / 255.0f);
    call.set_color_brightness(std::max(v[0], std::max(v[1], v[2])) / 255.0f);
    call.set_white(v[3] / 255.0f);
    call.set_brightness(v[4] / 255.0f);
  }
  call.perform();
  // The light writes the LED from its loop(). On an EVENT wake this runs during setup and the
  // candle sleeps before any loop() would run, so write it now.
  ls->loop();
}

// Parse "12 hr", "30 min", "8 sec (test)" into seconds; 0 if not a duration.
static int64_t parse_interval_s(const char *s) {
  while (*s == ' ') s++;
  int64_t v = 0;
  bool digits = false;
  while (*s >= '0' && *s <= '9') {
    v = v * 10 + (*s - '0');
    s++;
    digits = true;
  }
  while (*s == ' ') s++;
  if (!digits) return 0;
  switch (*s) {
    case 'h': case 'H': return v * 3600;
    case 'm': case 'M': return v * 60;
    case 's': case 'S': return v;
    default: return 0;
  }
}


// ===================================================================================================
// Part 2: the wake
// ===================================================================================================

using esphome::deep_sleep::DeepSleepComponent;
using esphome::light::LightState;
using esphome::sun::Sun;

static const char *const TAG = "candle";
static constexpr gpio_num_t DEBUG_PIN = GPIO_NUM_10;  // high while awake, for a scope

// Per-wake state. Zeroed at every boot.
struct Wake {
  // Milestones, millis() since boot (0 = not reached).
  uint32_t t_assoc, t_gotip, t_wifi, t_sntp, t_mqtt, t_led, t_interval;
  // Inputs received this wake.
  bool led_received, interval_received, schedule_received, debug_flag_received, adc_done, sntp_done;
  bool always_on;    // candles/sleep_interval == "always on" (OTA the old way)
  bool waketime_on;  // candles/debug_waketime == "on"
  bool sleeping;     // go_to_sleep started
  // SNTP drift measurement: the free-running clock just before the sync.
  int64_t sys_before_ms;
  uint32_t millis_before;
  int32_t drift_ms, drift_ppm;
  uint32_t ota_wait_until;   // millis() when the OTA window closes; 0 = no window
  int32_t event_early_s;     // EVENT wake came this early: sleep the rest
  uint32_t planned_sleep_s, t_sleep_wait;
};
static Wake wake;

static int64_t now_ms() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (int64_t) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static bool mqtt_up() {
  auto *c = esphome::mqtt::global_mqtt_client;
  return c != nullptr && c->is_connected();
}

// Publish to <device name>/<suffix>.
static bool publish(const char *suffix, const std::string &payload, uint8_t qos, bool retain) {
  if (!mqtt_up()) return false;
  std::string topic = esphome::App.get_name().str() + "/" + suffix;
  return esphome::mqtt::global_mqtt_client->publish(topic, payload, qos, retain);
}

static void own_ip(esp_netif_ip_info_t &ipi) {
  ipi = {};
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta != nullptr) esp_netif_get_ip_info(sta, &ipi);
}

// ---------------------------------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------------------------------

// on_boot 800: before WiFi and MQTT are set up.
static void boot_early() {
  decide_boot_kind();
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) correct_clock_after_sleep();
  // Drop the debug pin at the last moment before deep sleep (after ESPHome's shutdown and
  // teardown), so the scope pulse covers the whole wake.
  esp_deep_sleep_register_hook([]() { gpio_set_level(DEBUG_PIN, 0); });
  ESP_LOGI(TAG, "wake kind: %s", WAKE_NAMES[boot_kind]);
}

// on_boot 240: after WiFi setup created the default event loop, before association.
// Timestamps association and DHCP separately (ESPHome's wifi.connected means "has an IP").
static void register_wifi_hooks() {
  esp_err_t e1 = esp_event_handler_register(
      WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
      [](void *, esp_event_base_t, int32_t, void *) { if (wake.t_assoc == 0) wake.t_assoc = millis(); }, nullptr);
  esp_err_t e2 = esp_event_handler_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP,
      [](void *, esp_event_base_t, int32_t, void *) { if (wake.t_gotip == 0) wake.t_gotip = millis(); }, nullptr);
  if (e1 != ESP_OK || e2 != ESP_OK) ESP_LOGW(TAG, "event hook register failed: %d %d", e1, e2);
}

static void on_wifi_connected() {
  wake.t_wifi = millis();
  // Snapshot the free-running clock so the SNTP sync can measure its drift.
  wake.sys_before_ms = now_ms();
  wake.millis_before = millis();
  // ESPHome starts SNTP at setup, before WiFi; that first request fails and lwIP's backoff delays
  // the next one ~30 s. Ask again now that there's a route.
  esp_sntp_restart();
}

// <name>/wake: short health message, sent the moment MQTT is up (the earliest possible point).
static void on_mqtt_connected(float batt) {
  wake.t_mqtt = millis();
  wifi_ap_record_t ap = {};
  bool have_ap = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
  char msg[96];
  snprintf(msg, sizeof(msg), "kind=%s reset=%d rssi=%d ch=%u batt=%.2f", WAKE_NAMES[boot_kind],
           (int) esp_reset_reason(), have_ap ? (int) ap.rssi : 0, have_ap ? (unsigned) ap.primary : 0u,
           std::isnan(batt) ? 0.0f : batt);
  publish("wake", msg, 0, false);
}

// SNTP on_time_sync. Only a real sync counts: with the clock still valid after deep sleep, ESPHome's
// SNTP fires on_time_sync from its first loop(), before WiFi. millis_before is set once WiFi is up.
static void on_time_sync() {
  if (wake.sntp_done || wake.millis_before == 0) return;
  if (time_valid(wake.sys_before_ms / 1000)) {
    int64_t expected = wake.sys_before_ms + (int64_t) (millis() - wake.millis_before);
    wake.drift_ms = (int32_t) (now_ms() - expected);
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) wake.drift_ppm = calibrate(wake.drift_ms);
  }
  wake.t_sntp = millis();
  wake.sntp_done = true;
}

// ---------------------------------------------------------------------------------------------------
// MQTT inputs (all retained, so they arrive right after connecting)
// ---------------------------------------------------------------------------------------------------

// candles/led: HA decides. The retained light state overrides the schedule.
static void on_led(LightState *ls, JsonObjectConst x) {
  if (wake.t_led == 0) wake.t_led = millis();
  auto call = ls->make_call();
#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument doc;
#else
  DynamicJsonDocument doc(1024);
#endif
  JsonObject obj = doc.to<JsonObject>();
  obj.set(x);
  esphome::light::LightJSONSchema::parse_json(*ls, call, obj);
  call.perform();
  wake.led_received = true;
}

// candles/schedule: the rule, mirrored into RTC memory for offline on/off.
static void on_schedule(JsonObjectConst x) {
  Rule on{}, off{};
  if (parse_rule(x["on"] | "", on) && parse_rule(x["off"] | "", off)) {
    rtc.on = on;
    rtc.off = off;
    rtc.en = (x["en"] | 1) ? 1 : 0;
    JsonArrayConst look = x["look"];
    rtc.look_valid = look.size() == 5;
    for (size_t i = 0; rtc.look_valid && i < 5; i++) rtc.look[i] = (uint8_t) constrain(look[i] | 0, 0, 255);
  } else {
    ESP_LOGW(TAG, "candles/schedule: can't parse on/off, keeping the previous rule");
  }
  ESP_LOGI(TAG, "schedule en=%d on=%d/%d off=%d/%d look=%d:%u,%u,%u,%u,%u", rtc.en, rtc.on.kind,
           rtc.on.minutes, rtc.off.kind, rtc.off.minutes, rtc.look_valid, rtc.look[0], rtc.look[1],
           rtc.look[2], rtc.look[3], rtc.look[4]);
  wake.schedule_received = true;
}

// candles/sleep_interval: the max time between check-ins ("12 hr", "1 hr", ...), or "always on".
static void on_sleep_interval(const std::string &x, DeepSleepComponent *ds) {
  if (wake.t_interval == 0) wake.t_interval = millis();
  wake.always_on = strncmp(x.c_str(), "always on", 9) == 0;
  if (wake.always_on) {
    // Takes effect immediately and overrides the run_duration fallback until a normal interval arrives.
    ds->prevent_deep_sleep();
    ESP_LOGI(TAG, "sleep_interval 'always on': staying awake");
  } else {
    int64_t cap = parse_interval_s(x.c_str());
    if (cap > 0) rtc.cap_s = cap;
  }
  wake.interval_received = true;
}

// candles/debug_waketime: "on" = publish <name>/waketime at the end of each wake.
static void on_debug_waketime(const std::string &x) {
  wake.waketime_on = strcasecmp(x.c_str(), "on") == 0;
  wake.debug_flag_received = true;
}

// candles/ota_request: {"id":<unique>,"until":<epoch>,"window_s":900} from HA's "Candles OTA window"
// button. Each request is served once (served_id is kept in flash, so not again after the OTA
// reboot): stay awake up to window_s and publish <name>/ota_ready. The Device Builder sees the candle
// online and runs the queued upload.
static void on_ota_request(JsonObjectConst x, uint32_t &served_id, DeepSleepComponent *ds) {
  uint32_t req = x["id"] | 0u;
  int64_t until = x["until"] | (int64_t) 0;
  uint32_t window_s = std::clamp<uint32_t>(x["window_s"] | 900u, 60, 1800);
  if (req == 0 || req == served_id) return;
  time_t now = ::time(nullptr);
  if (time_valid(now) && until != 0 && now > until) {
    ESP_LOGI(TAG, "ota_request %u expired", (unsigned) req);
    return;
  }
  served_id = req;
  esphome::global_preferences->sync();
  wake.ota_wait_until = millis() + window_s * 1000;
  if (wake.ota_wait_until == 0) wake.ota_wait_until = 1;
  ds->prevent_deep_sleep();
  esp_netif_ip_info_t ipi;
  own_ip(ipi);
  char msg[96];
  snprintf(msg, sizeof(msg), "{\"ip\":\"" IPSTR "\",\"id\":%u,\"window_s\":%u}", IP2STR(&ipi.ip),
           (unsigned) req, (unsigned) window_s);
  publish("ota_ready", msg, 0, true);
  ESP_LOGI(TAG, "OTA window open for %us: %s", (unsigned) window_s, msg);
}

// ---------------------------------------------------------------------------------------------------
// Going to sleep
// ---------------------------------------------------------------------------------------------------

// Normal wake: ready once the retained inputs, the ADC sample and the clock are in. Also closes an
// OTA window that ran out without an upload.
static bool ready_to_sleep() {
  if (wake.sleeping || wake.t_mqtt == 0 || wake.always_on) return false;
  if (wake.ota_wait_until != 0) {
    if ((int32_t) (millis() - wake.ota_wait_until) < 0) return false;  // window open
    wake.ota_wait_until = 0;
    publish("ota_ready", "", 0, true);
    ESP_LOGI(TAG, "OTA window closed without an upload");
  }
  uint32_t since_mqtt = millis() - wake.t_mqtt;
  return wake.led_received && wake.interval_received && wake.adc_done &&
         // SNTP is best effort, but wait longer when the clock has never been set.
         (wake.sntp_done || since_mqtt > (time_valid(::time(nullptr)) ? 3000u : 10000u)) &&
         // Retained, so these come with the others; don't wait long if HA never published them.
         (wake.schedule_received || since_mqtt > 1000) && (wake.debug_flag_received || since_mqtt > 1000);
}

// WiFi or MQTT didn't come up: follow the mirrored schedule.
static void apply_offline(LightState *ls, Sun *sun) {
  time_t now = ::time(nullptr);
  if (schedule_known() && time_valid(now)) {
    bool on = scheduled_state(now, sun);
    ESP_LOGW(TAG, "offline: applying scheduled state %s", on ? "ON" : "OFF");
    // Switching on from off is the scheduled turn-on: use the look. Already on (HA set a color
    // manually): keep it.
    apply_state(ls, on, !ls->remote_values.is_on());
  } else {
    ESP_LOGW(TAG, "offline and no schedule/time known: leaving the light as is");
  }
}

// EVENT wake: apply the scheduled state, unless the clock says we're early (then event_early_s > 0).
static void apply_event(LightState *ls) {
  time_t now = ::time(nullptr);
  int64_t early = rtc.event_target - (int64_t) now;
  if (early > 1 && early < 600) {
    wake.event_early_s = (int32_t) early;
    return;
  }
  wake.event_early_s = 0;
  apply_state(ls, rtc.event_state != 0, true);  // the scheduled turn-on always uses the look
  rtc.last_event_target = rtc.event_target;
  rtc.last_event_actual = now;
}

// <name>/waketime: timing debug report (only with candles/debug_waketime "on").
// prev_pub: 0 = not sent, 1 = publish failed, 2 = sent but no ack before sleeping, 3 = acked.
static void publish_waketime(time_t now, int64_t next_ev, uint32_t sleep_s) {
  if (!wake.waketime_on || !mqtt_up()) {
    rtc.prev_pub = 0;
    return;
  }
  esp_netif_ip_info_t ipi;
  own_ip(ipi);
  char wt[400];
  snprintf(wt, sizeof(wt),
           "kind=%s assoc=%u gotip=%u wifi=%u sntp=%u mqtt=%u led=%u interval=%u sleep=%u ms since boot "
           "ip=" IPSTR " drift_ms=%d drift_ppm=%d scale_ppm=%d now=%lld next_event=%lld next=%s sleep_s=%u "
           "last_event=%lld last_event_actual=%lld n=%u prev_pub=%u prev_wait_ms=%u",
           WAKE_NAMES[boot_kind], (unsigned) wake.t_assoc, (unsigned) wake.t_gotip, (unsigned) wake.t_wifi,
           (unsigned) wake.t_sntp, (unsigned) wake.t_mqtt, (unsigned) wake.t_led, (unsigned) wake.t_interval,
           (unsigned) millis(), IP2STR(&ipi.ip), (int) wake.drift_ms, (int) wake.drift_ppm,
           (int) ((rtc.sleep_scale - 1.0) * 1e6), (long long) now, (long long) next_ev, WAKE_NAMES[rtc.next_kind],
           (unsigned) sleep_s, (long long) rtc.last_event_target, (long long) rtc.last_event_actual,
           (unsigned) rtc.wake_count, (unsigned) rtc.prev_pub, (unsigned) rtc.prev_wait_ms);
  ESP_LOGI("waketime", "%s", wt);
  rtc.prev_pub = publish("waketime", wt, 1, true) ? 2 : 1;
}

// Step 1 of going to sleep: plan the next wake and send the last messages. Returns false if a sleep
// is already under way.
static bool prepare_sleep(Sun *sun) {
  if (wake.sleeping) return false;
  wake.sleeping = true;
  // First sleep after a reboot (e.g. the one that follows an OTA): clear a leftover ota_ready.
  if (esp_reset_reason() != ESP_RST_DEEPSLEEP) publish("ota_ready", "", 1, true);
  time_t now = ::time(nullptr);
  int64_t next_ev = 0;
  wake.planned_sleep_s = plan_sleep(now, rtc.cap_s, sun, next_ev);
  publish_waketime(now, next_ev, wake.planned_sleep_s);
  wake.t_sleep_wait = millis();
  return true;
}

// Step 2: the last publishes are QoS 1; done when the broker acked them (or MQTT is down).
static bool last_publishes_done() {
  auto *c = esphome::mqtt::global_mqtt_client;
  return c == nullptr || !c->is_connected() || c->outbox_size() == 0;
}

// Step 3: sleep for the planned time (or `override_s` for an early EVENT wake).
static void sleep_now(DeepSleepComponent *ds, uint32_t override_s = 0) {
  uint32_t s = override_s ? override_s : wake.planned_sleep_s;
  if (s == 0) return;
  if (!override_s) {
    rtc.prev_wait_ms = (uint16_t) std::min<uint32_t>(millis() - wake.t_sleep_wait, 65535);
    if (rtc.prev_pub == 2 && last_publishes_done() && mqtt_up()) rtc.prev_pub = 3;
  } else {
    rtc.next_kind = WAKE_EVENT;
    wake.sleeping = true;
  }
  esp_deep_sleep_disable_rom_logging();  // no ESP-ROM banner on next wake (~30 ms at 115200)
  ds->allow_deep_sleep();
  ds->set_run_duration(0);
  ds->set_sleep_duration(begin_sleep_ms(s * 1000LL));
  ds->begin_sleep();
}

}  // namespace candle
