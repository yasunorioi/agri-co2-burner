// config.h — agri-co2-burner NVS-backed config.
//
// This node is the WiFi exception in the agri-* family (like agri-amp-wifi):
// the AtomHub Switch is a plain ATOM Lite behind an AC-DC supply, not an ATOM
// PoE, so there is no W5500 and core's agri::Network is NOT used — WiFi is
// brought up in main.cpp. We still reuse core's CommonConfig for MQTT/WebUI/OTA
// plumbing, and add the control-loop parameters here.
//
// CommonConfig re-use notes:
//   - common.mqtt_topic_prefix  → prefix WE publish our own state under
//     (e.g. "agriha/3"), <prefix>/actuator/CO2Burner.
//   - common.ccm_enabled        → repurposed as "listen to UECS-CCM as a
//     CO2/temp fallback source" (this node never SENDS CCM).

#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <AgriCommonConfig.h>

// Manual override mode. RAM-only intent, but persisted with a safety clamp:
// FORCE_ON never survives a reboot (an unattended node must not power a burner
// on boot). See loadConfig().
enum CtrlMode : uint8_t {
  MODE_AUTO      = 0,
  MODE_FORCE_OFF = 1,
  MODE_FORCE_ON  = 2,
};

struct AppConfig {
  agri::CommonConfig common;

  // --- data sources ---------------------------------------------------------
  // Topic = <src_prefix>/<src_category>/<type>. The category segment is
  // configurable because a house's readings are not always published under
  // "sensor": a node that speaks agriha natively lands in <house>/sensor/...,
  // but a house whose only sensor is an ArSprout/UECS node reaches MQTT solely
  // through the CCM bridge, which files it under <house>/sensor_ccm/... .
  // house2 is exactly that case (its CO2 is .80 via the bridge), so hardcoding
  // "sensor" left agri-co2-02 with no source at all.
  char     src_prefix[64];     // MQTT prefix of the CO2/temp source house
  char     src_category[16];   // "sensor" (native) or "sensor_ccm" (bridged)
  char     co2_type[20];       // UECS type; topic = <src_prefix>/<src_category>/<co2_type>
  char     temp_type[20];      // UECS type for temperature
  uint16_t src_stale_s;        // reading older than this = stale → fail-safe OFF

  // --- CO2 control (hysteresis around target) -------------------------------
  uint16_t co2_target;         // informational (display only)
  uint16_t co2_on_ppm;         // turn ON when CO2 <= this
  uint16_t co2_off_ppm;        // turn OFF when CO2 >= this
  uint16_t co2_hard_max_ppm;   // CO2 >= this → force OFF (CG-1000 spec: keep <=1000)

  // --- temperature gate (optional) ------------------------------------------
  bool     temp_gate;          // dose only when temp within [min,max]
  int16_t  temp_min_c10;       // *10 (e.g. 120 = 12.0 C)
  int16_t  temp_max_c10;       // *10

  // --- schedule (local JST, minutes from midnight) --------------------------
  bool     sched_enabled;
  uint16_t sched_start_min;    // inclusive
  uint16_t sched_end_min;      // exclusive; start>end means overnight wrap

  // --- window gate (poll Arsprout actuator position, read-only) -------------
  bool     win_gate;
  char     arsprout_host[40];
  uint16_t win_id1;            // component id (e.g. 64 = 東窓); 0 = unused
  uint16_t win_id2;            // component id (e.g. 65 = 西窓); 0 = unused
  uint8_t  win_open_pct;       // any window position > this % → suppress dosing
  uint16_t win_poll_s;
  uint16_t win_stale_s;        // window reading older than this = unknown
  bool     win_fail_open;      // window unknown → do NOT suppress (rely on schedule)

  // --- relay protection / duty ----------------------------------------------
  uint16_t relay_min_on_s;     // minimum ON dwell once fired (anti short-cycle)
  uint16_t relay_min_off_s;    // minimum OFF dwell before re-firing
  uint8_t  duty_pct_max;       // max % of duty window burner may be ON (spec <=50)
  uint16_t duty_window_s;      // duty accounting window (tumbling)

  // --- WiFi (STA; SoftAP provisioning fallback in main.cpp) -----------------
  char     wifi_ssid[32];
  char     wifi_pass[64];

  // --- runtime (not persisted as FORCE_ON) ----------------------------------
  uint8_t  mode;               // CtrlMode
};

extern AppConfig g_cfg;

// UNIX seconds if SNTP has synced, else 0 (consumers treat 0 as "no clock").
// Note: time(nullptr) is UTC regardless of the JST tz set in main.cpp; the
// schedule uses localtime_r() for wall-clock, this stays UTC for {…,ts}.
inline long nowEpoch() {
  time_t t = time(nullptr);
  return (t > 1700000000) ? (long)t : 0;
}

inline void setDefaults() {
  agri::commonDefaults(g_cfg.common,
                       "co2brn_01", "agri-co2-01",
                       /*mqtt prefix = this house scope*/ "agriha/3",
                       /*default_ccm_region*/ 13);
  g_cfg.common.ccm_enabled = false;   // CCM here = fallback RECEIVE, off by default

  strlcpy(g_cfg.src_prefix, "agriha/3", sizeof(g_cfg.src_prefix));
  strlcpy(g_cfg.src_category, "sensor", sizeof(g_cfg.src_category));
  strlcpy(g_cfg.co2_type,  "InAirCO2",  sizeof(g_cfg.co2_type));
  strlcpy(g_cfg.temp_type, "InAirTemp", sizeof(g_cfg.temp_type));
  g_cfg.src_stale_s = 180;

  g_cfg.co2_target       = 500;
  g_cfg.co2_on_ppm       = 450;
  g_cfg.co2_off_ppm      = 520;
  g_cfg.co2_hard_max_ppm = 1000;   // CG-1000: 庫内 1000ppm 以下

  g_cfg.temp_gate    = false;
  g_cfg.temp_min_c10 = 120;        // 12.0 C
  g_cfg.temp_max_c10 = 320;        // 32.0 C

  g_cfg.sched_enabled   = true;
  g_cfg.sched_start_min = 8 * 60;  // 08:00 — 側窓が開く前の午前を既定に（要調整）
  g_cfg.sched_end_min   = 15 * 60; // 15:00

  g_cfg.win_gate = true;
  strlcpy(g_cfg.arsprout_host, "192.168.1.81", sizeof(g_cfg.arsprout_host));
  // ArSprout component ids for the side windows, all served by .81. Verified
  // against GET /api/component (CcmRegion/CcmOrder ↔ id), because pointing a
  // node at the wrong house's windows is silent — the gate still reads fresh:
  //   region 71 order 1,2 → id 32 側窓東2 / 33 側窓西2   (house2)
  //   region 72 order 1,2 → id 64 側窓東3 / 65 側窓西3   (house3)
  // (house1's windows are region 61 on .71, the older ArSprout, not here.)
  g_cfg.win_id1       = 64;        // h3 東窓 — defaults target house3
  g_cfg.win_id2       = 65;        // h3 西窓
  g_cfg.win_open_pct  = 5;         // >5% 開 = 撒かない
  g_cfg.win_poll_s    = 30;
  g_cfg.win_stale_s   = 120;
  g_cfg.win_fail_open = true;      // Arsprout 不達時はスケジュールに委ねる

  g_cfg.relay_min_on_s  = 300;     // 5 min
  g_cfg.relay_min_off_s = 300;     // 5 min
  g_cfg.duty_pct_max    = 50;      // CG-1000: 毎時デューティ 50% 以下
  g_cfg.duty_window_s   = 3600;

  strlcpy(g_cfg.wifi_ssid, "", sizeof(g_cfg.wifi_ssid));
  strlcpy(g_cfg.wifi_pass, "", sizeof(g_cfg.wifi_pass));

  g_cfg.mode = MODE_AUTO;
}

inline void loadConfig() {
  setDefaults();
  Preferences p;
  if (!p.begin("co2brn", true)) return;
  agri::commonLoad(g_cfg.common, p);

  auto loadStr = [&](const char *key, char *dst, size_t n) {
    String v = p.getString(key, dst);
    strlcpy(dst, v.c_str(), n);
  };
  loadStr("src_pfx",  g_cfg.src_prefix, sizeof(g_cfg.src_prefix));
  loadStr("src_cat",  g_cfg.src_category, sizeof(g_cfg.src_category));
  loadStr("co2_ty",   g_cfg.co2_type,   sizeof(g_cfg.co2_type));
  loadStr("temp_ty",  g_cfg.temp_type,  sizeof(g_cfg.temp_type));
  g_cfg.src_stale_s     = p.getUShort("src_stl",  g_cfg.src_stale_s);

  g_cfg.co2_target       = p.getUShort("co2_tgt", g_cfg.co2_target);
  g_cfg.co2_on_ppm       = p.getUShort("co2_on",  g_cfg.co2_on_ppm);
  g_cfg.co2_off_ppm      = p.getUShort("co2_off", g_cfg.co2_off_ppm);
  g_cfg.co2_hard_max_ppm = p.getUShort("co2_max", g_cfg.co2_hard_max_ppm);

  g_cfg.temp_gate    = p.getBool ("t_gate", g_cfg.temp_gate);
  g_cfg.temp_min_c10 = p.getShort("t_min",  g_cfg.temp_min_c10);
  g_cfg.temp_max_c10 = p.getShort("t_max",  g_cfg.temp_max_c10);

  g_cfg.sched_enabled   = p.getBool  ("sc_en",  g_cfg.sched_enabled);
  g_cfg.sched_start_min = p.getUShort("sc_beg", g_cfg.sched_start_min);
  g_cfg.sched_end_min   = p.getUShort("sc_end", g_cfg.sched_end_min);

  g_cfg.win_gate = p.getBool("w_gate", g_cfg.win_gate);
  loadStr("w_host", g_cfg.arsprout_host, sizeof(g_cfg.arsprout_host));
  g_cfg.win_id1       = p.getUShort("w_id1",  g_cfg.win_id1);
  g_cfg.win_id2       = p.getUShort("w_id2",  g_cfg.win_id2);
  g_cfg.win_open_pct  = p.getUChar ("w_open", g_cfg.win_open_pct);
  g_cfg.win_poll_s    = p.getUShort("w_poll", g_cfg.win_poll_s);
  g_cfg.win_stale_s   = p.getUShort("w_stl",  g_cfg.win_stale_s);
  g_cfg.win_fail_open = p.getBool  ("w_fopen",g_cfg.win_fail_open);

  g_cfg.relay_min_on_s  = p.getUShort("r_mon",  g_cfg.relay_min_on_s);
  g_cfg.relay_min_off_s = p.getUShort("r_moff", g_cfg.relay_min_off_s);
  g_cfg.duty_pct_max    = p.getUChar ("d_max",  g_cfg.duty_pct_max);
  g_cfg.duty_window_s   = p.getUShort("d_win",  g_cfg.duty_window_s);

  loadStr("wifi_ss", g_cfg.wifi_ssid, sizeof(g_cfg.wifi_ssid));
  loadStr("wifi_pw", g_cfg.wifi_pass, sizeof(g_cfg.wifi_pass));

  // Persisted mode, but FORCE_ON is downgraded to AUTO on boot so an
  // unattended reboot never powers the burner on.
  g_cfg.mode = p.getUChar("mode", g_cfg.mode);
  if (g_cfg.mode == MODE_FORCE_ON) g_cfg.mode = MODE_AUTO;

  p.end();
}

inline bool saveConfig() {
  Preferences p;
  if (!p.begin("co2brn", false)) return false;
  agri::commonSave(g_cfg.common, p);

  p.putString("src_pfx", g_cfg.src_prefix);
  p.putString("src_cat", g_cfg.src_category);
  p.putString("co2_ty",  g_cfg.co2_type);
  p.putString("temp_ty", g_cfg.temp_type);
  p.putUShort("src_stl", g_cfg.src_stale_s);

  p.putUShort("co2_tgt", g_cfg.co2_target);
  p.putUShort("co2_on",  g_cfg.co2_on_ppm);
  p.putUShort("co2_off", g_cfg.co2_off_ppm);
  p.putUShort("co2_max", g_cfg.co2_hard_max_ppm);

  p.putBool ("t_gate", g_cfg.temp_gate);
  p.putShort("t_min",  g_cfg.temp_min_c10);
  p.putShort("t_max",  g_cfg.temp_max_c10);

  p.putBool  ("sc_en",  g_cfg.sched_enabled);
  p.putUShort("sc_beg", g_cfg.sched_start_min);
  p.putUShort("sc_end", g_cfg.sched_end_min);

  p.putBool  ("w_gate", g_cfg.win_gate);
  p.putString("w_host", g_cfg.arsprout_host);
  p.putUShort("w_id1",  g_cfg.win_id1);
  p.putUShort("w_id2",  g_cfg.win_id2);
  p.putUChar ("w_open", g_cfg.win_open_pct);
  p.putUShort("w_poll", g_cfg.win_poll_s);
  p.putUShort("w_stl",  g_cfg.win_stale_s);
  p.putBool  ("w_fopen",g_cfg.win_fail_open);

  p.putUShort("r_mon",  g_cfg.relay_min_on_s);
  p.putUShort("r_moff", g_cfg.relay_min_off_s);
  p.putUChar ("d_max",  g_cfg.duty_pct_max);
  p.putUShort("d_win",  g_cfg.duty_window_s);

  p.putString("wifi_ss", g_cfg.wifi_ssid);
  p.putString("wifi_pw", g_cfg.wifi_pass);

  p.putUChar ("mode", g_cfg.mode);
  p.end();
  return true;
}
