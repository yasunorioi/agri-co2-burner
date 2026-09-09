// webui_hooks.h — dashboard block, config-form rows, form apply, and the
// /api/status augmentation for agri-co2-burner. Wired into core's WebUI via
// the agri::WebHooks struct in main.cpp.
//
// NOTE: core's WebUI "Network" box + About page read ETH.localIP()/macAddress()
// which are empty on this WiFi node — the real WiFi IP/RSSI are surfaced here
// in our own dashboard block and in addStatusFields() instead.

#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <AgriNode.h>
#include "config.h"
#include "sources.h"
#include "control.h"

static const char *modeName(uint8_t m) {
  switch (m) {
    case MODE_AUTO:      return "AUTO";
    case MODE_FORCE_OFF: return "FORCE OFF";
    case MODE_FORCE_ON:  return "FORCE ON";
  }
  return "?";
}

inline String hmm(uint16_t minutes) {
  char b[8];
  snprintf(b, sizeof(b), "%02u:%02u", minutes / 60, minutes % 60);
  return String(b);
}

// Parse an <input type=time> value ("HH:MM", ':' arrives url-decoded) into
// minutes-from-midnight; return defMin on empty/invalid input.
inline int parseHHMM(const String &v, int defMin) {
  int c = v.indexOf(':');
  if (c < 0) return defMin;
  int hh = v.substring(0, c).toInt();
  int mm = v.substring(c + 1).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return defMin;
  return hh * 60 + mm;
}

// ---- Dashboard -------------------------------------------------------------
inline String renderDashboard() {
  String s; s.reserve(1400);
  char buf[24];

  // Big relay state banner
  s += F("<h3>CO&#8322; Burner (CG-1000)</h3><table>");
  s += "<tr><th>Relay</th><td><b style='color:";
  s += g_relay ? "#fc6" : "#7c7";
  s += "'>"; s += g_relay ? "ON — burner enabled" : "OFF";
  s += F("</b></td></tr>");
  s += "<tr><th>Mode</th><td>"; s += modeName(g_cfg.mode); s += "</td></tr>";
  s += "<tr><th>Reason</th><td>"; s += g_reason ? g_reason : "-"; s += "</td></tr>";
  s += "<tr><th>Duty (this window)</th><td>"; s += dutyPctNow();
  s += " % of "; s += g_cfg.duty_pct_max; s += F(" % max</td></tr>");
  s += F("</table>");

  // Inputs
  s += F("<h3>Inputs</h3><table>");
  s += "<tr><th>CO&#8322;</th><td>";
  if (g_co2_ms) {
    dtostrf(g_co2_ppm, 1, 0, buf); s += buf;
    s += " ppm <span style='color:#9aa'>(src="; s += g_co2_src;
    s += ", "; s += (millis() - g_co2_ms) / 1000; s += "s ago)</span>";
    if (!co2Fresh()) s += F(" <b style='color:#f88'>STALE</b>");
  } else s += F("<span style='color:#f88'>no reading</span>");
  s += F("</td></tr>");
  s += "<tr><th>On / Off / Max</th><td>"; s += g_cfg.co2_on_ppm; s += " / ";
  s += g_cfg.co2_off_ppm; s += " / "; s += g_cfg.co2_hard_max_ppm; s += F(" ppm</td></tr>");

  s += "<tr><th>Temp</th><td>";
  if (g_temp_ms) { dtostrf(g_temp_c, 1, 1, buf); s += buf; s += " &deg;C";
    if (!tempFresh()) s += F(" <b style='color:#f88'>STALE</b>"); }
  else s += "-";
  if (g_cfg.temp_gate) {
    s += F(" <span style='color:#9aa'>(gate ");
    dtostrf(g_cfg.temp_min_c10 / 10.0f, 1, 1, buf); s += buf; s += "–";
    dtostrf(g_cfg.temp_max_c10 / 10.0f, 1, 1, buf); s += buf; s += F(" &deg;C)</span>");
  } else s += F(" <span style='color:#9aa'>(gate off)</span>");
  s += F("</td></tr>");

  s += "<tr><th>Side window</th><td>";
  if (!g_cfg.win_gate) s += F("<span style='color:#9aa'>gate disabled</span>");
  else if (winFresh()) { s += g_win_pct; s += " % ("; s += g_win_mode;
    s += ", "; s += (millis() - g_win_ms) / 1000; s += "s ago)";
    if (g_win_pct > g_cfg.win_open_pct) s += F(" <b style='color:#fc6'>OPEN</b>"); }
  else s += F("<span style='color:#f88'>unknown</span>");
  s += F("</td></tr>");

  s += "<tr><th>Schedule</th><td>";
  if (!g_cfg.sched_enabled) s += F("always");
  else { s += hmm(g_cfg.sched_start_min); s += "–"; s += hmm(g_cfg.sched_end_min);
    s += withinSchedule() ? F(" <span style='color:#7c7'>(active)</span>")
                          : F(" <span style='color:#9aa'>(inactive)</span>"); }
  s += F("</td></tr></table>");

  return s;
}

// ---- Config rows (appended to core's Config page) --------------------------
inline String renderConfigRows() {
  String s; s.reserve(2600);
  auto sec = [&](const char *t) {
    s += F("<tr><th colspan=2 style='background:#1a1a1f;color:#9ad;text-align:left;padding:8px'>");
    s += t; s += F("</th></tr>");
  };
  auto num = [&](const char *label, const char *name, long val) {
    s += "<tr><th>"; s += label; s += "</th><td><input type=number name="; s += name;
    s += " value='"; s += val; s += "'></td></tr>";
  };
  auto txt = [&](const char *label, const char *name, const char *val) {
    s += "<tr><th>"; s += label; s += "</th><td><input name="; s += name;
    s += " value='"; s += val; s += "'></td></tr>";
  };
  auto chk = [&](const char *label, const char *name, bool val) {
    s += "<tr><th>"; s += label; s += "</th><td><input type=checkbox name="; s += name;
    if (val) s += " checked"; s += "></td></tr>";
  };

  sec("Mode");
  s += F("<tr><th>Control mode</th><td><select name=mode>");
  const uint8_t modes[] = {MODE_AUTO, MODE_FORCE_OFF, MODE_FORCE_ON};
  for (uint8_t m : modes) {
    s += "<option value="; s += m; s += (g_cfg.mode == m ? " selected>" : ">");
    s += modeName(m); s += "</option>";
  }
  s += F("</select> <span style='color:#888'>FORCE ON は再起動で AUTO に戻る</span></td></tr>");

  sec("Data source");
  txt("Source MQTT prefix", "src_pfx", g_cfg.src_prefix);
  txt("Source category", "src_cat", g_cfg.src_category);
  s += F("<tr><td colspan=2 style='color:#888;font-size:85%'>"
         "購読先 = &lt;prefix&gt;/&lt;category&gt;/&lt;type&gt;。"
         "native な agriha ノードがあるハウスは <code>sensor</code>、"
         "CCM ブリッジ経由でしか値が来ないハウスは <code>sensor_ccm</code>。</td></tr>");
  txt("CO2 UECS type",  "co2_ty",  g_cfg.co2_type);
  txt("Temp UECS type", "temp_ty", g_cfg.temp_type);
  num("Stale timeout (s)", "src_stl", g_cfg.src_stale_s);

  sec("CO2 setpoints (ppm)");
  num("Target (display)", "co2_tgt", g_cfg.co2_target);
  num("ON when CO2 <=",   "co2_on",  g_cfg.co2_on_ppm);
  num("OFF when CO2 >=",  "co2_off", g_cfg.co2_off_ppm);
  num("Hard max (force OFF)", "co2_max", g_cfg.co2_hard_max_ppm);

  sec("Temperature gate (optional)");
  chk("Enable temp gate", "t_gate", g_cfg.temp_gate);
  num("Temp min (x10 C)", "t_min", g_cfg.temp_min_c10);
  num("Temp max (x10 C)", "t_max", g_cfg.temp_max_c10);

  sec("Schedule (JST)");
  chk("Enable schedule", "sc_en", g_cfg.sched_enabled);
  s += "<tr><th>Start</th><td><input type=time name=sc_beg value='"
     + hmm(g_cfg.sched_start_min) + "'></td></tr>";
  s += "<tr><th>End</th><td><input type=time name=sc_end value='"
     + hmm(g_cfg.sched_end_min) + "'></td></tr>";
  s += F("<tr><td colspan=2 style='color:#888;font-size:85%'>"
         "開始&gt;終了で日跨ぎ (夜間) 撒布</td></tr>");

  sec("Side-window gate (ArSprout poll)");
  chk("Enable window gate", "w_gate", g_cfg.win_gate);
  txt("ArSprout host", "w_host", g_cfg.arsprout_host);
  num("Window id 1 (0=off)", "w_id1", g_cfg.win_id1);
  num("Window id 2 (0=off)", "w_id2", g_cfg.win_id2);
  num("Open threshold (%)",  "w_open", g_cfg.win_open_pct);
  num("Poll interval (s)",   "w_poll", g_cfg.win_poll_s);
  num("Window stale (s)",    "w_stl",  g_cfg.win_stale_s);
  chk("Fail-open (unknown → allow)", "w_fopen", g_cfg.win_fail_open);

  sec("Relay protection / duty");
  num("Min ON (s)",  "r_mon",  g_cfg.relay_min_on_s);
  num("Min OFF (s)", "r_moff", g_cfg.relay_min_off_s);
  num("Duty max (%)", "d_max", g_cfg.duty_pct_max);
  num("Duty window (s)", "d_win", g_cfg.duty_window_s);

  // WiFi credentials are owned by WiFiManager (join agri-co2-setup / hold the
  // button at boot to re-provision), not this form.

  return s;
}

// ---- apply config form -----------------------------------------------------
inline void applyConfigRows(const String &b) {
  g_cfg.mode = (uint8_t)agri::parseFormInt(b, "mode", g_cfg.mode);

  agri::parseFormStr(b, "src_pfx", g_cfg.src_prefix, sizeof(g_cfg.src_prefix));
  agri::parseFormStr(b, "src_cat", g_cfg.src_category, sizeof(g_cfg.src_category));
  agri::parseFormStr(b, "co2_ty",  g_cfg.co2_type,   sizeof(g_cfg.co2_type));
  agri::parseFormStr(b, "temp_ty", g_cfg.temp_type,  sizeof(g_cfg.temp_type));
  g_cfg.src_stale_s     = (uint16_t)agri::parseFormInt(b, "src_stl", g_cfg.src_stale_s);

  g_cfg.co2_target       = (uint16_t)agri::parseFormInt(b, "co2_tgt", g_cfg.co2_target);
  g_cfg.co2_on_ppm       = (uint16_t)agri::parseFormInt(b, "co2_on",  g_cfg.co2_on_ppm);
  g_cfg.co2_off_ppm      = (uint16_t)agri::parseFormInt(b, "co2_off", g_cfg.co2_off_ppm);
  g_cfg.co2_hard_max_ppm = (uint16_t)agri::parseFormInt(b, "co2_max", g_cfg.co2_hard_max_ppm);

  g_cfg.temp_gate    = agri::parseFormBool(b, "t_gate");
  g_cfg.temp_min_c10 = (int16_t)agri::parseFormInt(b, "t_min", g_cfg.temp_min_c10);
  g_cfg.temp_max_c10 = (int16_t)agri::parseFormInt(b, "t_max", g_cfg.temp_max_c10);

  g_cfg.sched_enabled   = agri::parseFormBool(b, "sc_en");
  char tb[8];
  tb[0] = 0; agri::parseFormStr(b, "sc_beg", tb, sizeof(tb));
  g_cfg.sched_start_min = (uint16_t)parseHHMM(String(tb), g_cfg.sched_start_min);
  tb[0] = 0; agri::parseFormStr(b, "sc_end", tb, sizeof(tb));
  g_cfg.sched_end_min   = (uint16_t)parseHHMM(String(tb), g_cfg.sched_end_min);

  g_cfg.win_gate = agri::parseFormBool(b, "w_gate");
  agri::parseFormStr(b, "w_host", g_cfg.arsprout_host, sizeof(g_cfg.arsprout_host));
  g_cfg.win_id1      = (uint16_t)agri::parseFormInt(b, "w_id1", g_cfg.win_id1);
  g_cfg.win_id2      = (uint16_t)agri::parseFormInt(b, "w_id2", g_cfg.win_id2);
  g_cfg.win_open_pct = (uint8_t) agri::parseFormInt(b, "w_open", g_cfg.win_open_pct);
  g_cfg.win_poll_s   = (uint16_t)agri::parseFormInt(b, "w_poll", g_cfg.win_poll_s);
  g_cfg.win_stale_s  = (uint16_t)agri::parseFormInt(b, "w_stl",  g_cfg.win_stale_s);
  g_cfg.win_fail_open= agri::parseFormBool(b, "w_fopen");

  g_cfg.relay_min_on_s  = (uint16_t)agri::parseFormInt(b, "r_mon",  g_cfg.relay_min_on_s);
  g_cfg.relay_min_off_s = (uint16_t)agri::parseFormInt(b, "r_moff", g_cfg.relay_min_off_s);
  g_cfg.duty_pct_max    = (uint8_t) agri::parseFormInt(b, "d_max",  g_cfg.duty_pct_max);
  g_cfg.duty_window_s   = (uint16_t)agri::parseFormInt(b, "d_win",  g_cfg.duty_window_s);

}

// ---- /api/status augmentation ----------------------------------------------
inline void addStatusFields(JsonObject d) {
  d["wifi_ip"]   = WiFi.localIP().toString();
  d["wifi_rssi"] = WiFi.RSSI();
  d["relay"]     = g_relay;
  d["mode"]      = modeName(g_cfg.mode);
  d["reason"]    = g_reason ? g_reason : "-";
  d["duty_pct"]  = dutyPctNow();
  if (g_co2_ms)  { d["co2_ppm"] = g_co2_ppm; d["co2_src"] = g_co2_src;
                   d["co2_fresh"] = co2Fresh(); }
  if (g_temp_ms) d["temp_c"] = g_temp_c;
  if (g_cfg.win_gate) { d["win_pct"] = g_win_pct; d["win_fresh"] = winFresh(); }
}
