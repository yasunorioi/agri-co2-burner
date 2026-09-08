// control.h — the autonomous CO2-dosing decision + relay drive.
//
// Relay wiring (M5 AtomHub Switch / K042, ATOM Lite inside):
//   RELAY1 = GPIO22 (active HIGH) → CG-1000 burner enable contact
//   RELAY2 = GPIO19 (spare)
// When RELAY1 closes, the CG-1000's own switch is allowed to fire the burner.
//
// Decision priority (highest first), evaluated every controlTick():
//   S1  FORCE_OFF, CO2 stale (fail-safe), or CO2 >= hard-max   → OFF now
//   S2  duty budget exhausted                                  → OFF now
//   S3  min-off dwell not elapsed                              → stay OFF
//   S4  min-on dwell not elapsed (and currently ON)            → hold ON
//   N   normal: mode + CO2 hysteresis + schedule + window + temp
//
// S1/S2 are safety/spec limits and override the S4 min-on hold. This is a
// combustion device: when in doubt, OFF.

#pragma once

#include <Arduino.h>
#include <time.h>
#include "config.h"
#include "sources.h"

static const int PIN_RELAY1 = 22;   // CG-1000 burner enable
static const int PIN_RELAY2 = 19;   // spare

// ---- live control state ----------------------------------------------------
extern bool        g_relay;         // current relay state (true = ON)
extern uint32_t    g_relay_ms;      // millis() of last relay transition
extern const char *g_reason;        // human-readable last decision reason

// duty accounting (tumbling window)
extern uint32_t    g_duty_win_start_ms;
extern uint32_t    g_duty_on_ms;    // accumulated ON time in current window
extern uint32_t    g_duty_last_ms;  // last tick millis for delta integration

inline void relayWrite(bool on) {
  if (on == g_relay) return;
  g_relay = on;
  g_relay_ms = millis();
  digitalWrite(PIN_RELAY1, on ? HIGH : LOW);
  Serial.printf("[RELAY] %s (%s)\n", on ? "ON" : "OFF", g_reason ? g_reason : "");
}

inline void controlBegin() {
  pinMode(PIN_RELAY1, OUTPUT);
  pinMode(PIN_RELAY2, OUTPUT);
  digitalWrite(PIN_RELAY1, LOW);
  digitalWrite(PIN_RELAY2, LOW);
  g_relay = false;
  g_relay_ms = millis();
  g_duty_win_start_ms = millis();
  g_duty_on_ms = 0;
  g_duty_last_ms = millis();
}

// Local wall-clock minutes-from-midnight, or -1 if SNTP not synced.
inline int localMinuteOfDay() {
  if (nowEpoch() == 0) return -1;
  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  return tmv.tm_hour * 60 + tmv.tm_min;
}

inline bool withinSchedule() {
  if (!g_cfg.sched_enabled) return true;
  int m = localMinuteOfDay();
  if (m < 0) return false;                      // no clock → treat as outside (safe)
  int a = g_cfg.sched_start_min, b = g_cfg.sched_end_min;
  if (a == b) return false;
  if (a < b)  return m >= a && m < b;            // same-day window
  return m >= a || m < b;                        // overnight wrap
}

// Window closed enough to dose? Suppress when any polled window is open past
// win_open_pct. When the reading is stale/unknown, win_fail_open decides.
inline bool windowClosed() {
  if (!g_cfg.win_gate) return true;
  if (!winFresh())     return g_cfg.win_fail_open;   // unknown → rely on schedule (default)
  return g_win_pct <= g_cfg.win_open_pct;
}

inline bool tempOk() {
  if (!g_cfg.temp_gate) return true;
  if (!tempFresh())     return false;                // gate on but no temp → don't dose
  int c10 = (int)lroundf(g_temp_c * 10.0f);
  return c10 >= g_cfg.temp_min_c10 && c10 <= g_cfg.temp_max_c10;
}

inline uint8_t dutyPctNow() {
  if (g_cfg.duty_window_s == 0) return 0;
  return (uint8_t)((uint64_t)g_duty_on_ms * 100ULL /
                   ((uint32_t)g_cfg.duty_window_s * 1000ULL));
}

inline bool dutyExhausted() {
  return dutyPctNow() >= g_cfg.duty_pct_max;
}

// Integrate ON-time into the tumbling duty window; roll the window over.
inline void dutyIntegrate(uint32_t now) {
  if (now - g_duty_win_start_ms >= (uint32_t)g_cfg.duty_window_s * 1000UL) {
    g_duty_win_start_ms = now;
    g_duty_on_ms = 0;
  }
  if (g_relay) g_duty_on_ms += (now - g_duty_last_ms);
  g_duty_last_ms = now;
}

inline void controlTick(uint32_t now) {
  dutyIntegrate(now);

  bool min_off_ok = (now - g_relay_ms) >= (uint32_t)g_cfg.relay_min_off_s * 1000UL;
  bool min_on_ok  = (now - g_relay_ms) >= (uint32_t)g_cfg.relay_min_on_s  * 1000UL;

  // --- S1: hard safety OFF (overrides everything, incl. min-on hold) --------
  if (g_cfg.mode == MODE_FORCE_OFF) { g_reason = "manual FORCE OFF"; relayWrite(false); return; }
  if (!co2Fresh())                  { g_reason = "CO2 stale → fail-safe OFF"; relayWrite(false); return; }
  if (g_co2_ppm >= g_cfg.co2_hard_max_ppm) { g_reason = "CO2 >= hard-max"; relayWrite(false); return; }

  // --- S2: duty budget (spec limit) ----------------------------------------
  if (dutyExhausted()) { g_reason = "duty budget exhausted"; relayWrite(false); return; }

  // --- compute demand -------------------------------------------------------
  bool demand;
  if (g_cfg.mode == MODE_FORCE_ON) {
    demand = true;                                 // still bounded by S1/S2 above
    g_reason = "manual FORCE ON";
  } else {
    // CO2 hysteresis: fire below on-point, release above off-point.
    bool co2_calls = g_relay ? (g_co2_ppm < g_cfg.co2_off_ppm)
                             : (g_co2_ppm <= g_cfg.co2_on_ppm);
    bool sched = withinSchedule();
    bool win   = windowClosed();
    bool temp  = tempOk();
    demand = co2_calls && sched && win && temp;

    if      (!co2_calls) g_reason = g_relay ? "CO2 >= off-point" : "CO2 above on-point";
    else if (!sched)     g_reason = "outside schedule";
    else if (!win)       g_reason = "side window open";
    else if (!temp)      g_reason = "temp out of range";
    else                 g_reason = "CO2 low → dosing";
  }

  // --- apply dwell limits ---------------------------------------------------
  if (demand) {
    if (!g_relay && !min_off_ok) { g_reason = "min-off dwell"; return; }  // want on, must wait
    relayWrite(true);
  } else {
    if (g_relay && !min_on_ok) { g_reason = "min-on hold"; return; }      // want off, hold min-on
    relayWrite(false);
  }
}
