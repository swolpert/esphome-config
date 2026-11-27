#pragma once
// Schedule mirroring and wake planning for candleDev10.yaml.
//
// HA publishes a retained rule on candles/schedule, e.g. {"en":1,"on":"sunset-30","off":"22:00"}.
// The candle keeps the parsed rule in RTC memory (survives deep sleep, lost on battery swap), so it
// can switch on/off at the scheduled times by itself. HA's retained candles/led still overrides it
// on every wake that reaches MQTT.
//
// Wake kinds:
//   CHECK     - routine check-in (WiFi + SNTP + MQTT), at most `cap` apart
//   PRE_EVENT - timer wake aimed a little before an event, to resync the clock (WiFi + SNTP + MQTT)
//   EVENT     - precise timer wake at the event: apply the scheduled state, no WiFi, sleep again

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#include "esphome/core/defines.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/sun/sun.h"
#include "esphome/components/time/posix_tz.h"

namespace candle {

enum WakeKind : uint8_t { WAKE_CHECK = 0, WAKE_PRE_EVENT = 1, WAKE_EVENT = 2 };
static const char *const WAKE_NAMES[] = {"check", "pre", "event"};

enum RuleKind : uint8_t { RULE_NONE = 0, RULE_TIME = 1, RULE_SUNSET = 2, RULE_SUNRISE = 3 };
struct Rule {
  uint8_t kind;
  int16_t minutes;  // RULE_TIME: minutes after local midnight; sun rules: offset in minutes
};

static constexpr uint32_t RTC_MAGIC = 0xCA9D1E10;
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
// shutdown), so switching on/off from the schedule only has to change the state.
static void apply_state(esphome::light::LightState *ls, bool on) {
  auto call = ls->make_call();
  call.set_state(on);
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

}  // namespace candle
