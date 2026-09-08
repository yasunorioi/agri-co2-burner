// sources.h — CO2 / temperature / window-position acquisition.
//
// Three independent inputs, each with its own freshness stamp so the control
// loop can fail safe when any goes stale:
//   1. MQTT  (primary)  — subscribe <src_prefix>/sensor/<co2_type|temp_type>,
//                         agriha-native {value,unit,ts} payloads.
//   2. CCM   (fallback) — listen on UDP 16520 for <DATA type="<type>.cMC">…,
//                         which is what ArSprout (and agri-env-poe with CCM
//                         enabled) broadcast. Only used when common.ccm_enabled.
//   3. Window (gate)    — poll ArSprout's read-only actuator status endpoint
//                         GET /api/component/<id> → {value:0..100, mode}.
//                         This is NOT the operate endpoint (no side effect).

#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkUdp.h>
#include <ArduinoJson.h>
#include <AgriNode.h>
#include "config.h"

// ---- live values (updated by callbacks/polls, read by control) -------------
extern float    g_co2_ppm;      // NAN until first reading
extern uint32_t g_co2_ms;       // millis() of last CO2 update (0 = never)
extern const char *g_co2_src;   // "mqtt" / "ccm" / "-"
extern float    g_temp_c;       // NAN until first reading
extern uint32_t g_temp_ms;

extern int      g_win_pct;      // max of polled windows, -1 = unknown
extern uint32_t g_win_ms;       // millis() of last successful window poll
extern char     g_win_mode[12]; // ArSprout mode of the last polled window

// ArSprout Basic auth: admin with empty password (see reference_arsprout).
// base64("admin:") = "YWRtaW46".
static const char *ARSPROUT_AUTH = "Basic YWRtaW46";

// ---- topic strings built once at begin() -----------------------------------
inline String &co2Topic()  { static String s; return s; }
inline String &tempTopic() { static String s; return s; }

// ---- CCM fallback listener -------------------------------------------------
inline NetworkUDP &ccmRxSocket() { static NetworkUDP u; return u; }

// Extract the text content of the first <DATA type="<typePrefix>..."> element.
// Matches on a type PREFIX so "InAirCO2", "InAirCO2.cMC", "InAirCO2.cMC" with
// any room/region all hit. Returns true and fills `out` on success.
inline bool ccmExtract(const String &xml, const char *typePrefix, float &out) {
  String needle = String("type=\"") + typePrefix;
  int p = xml.indexOf(needle);
  if (p < 0) return false;
  int gt = xml.indexOf('>', p);
  if (gt < 0) return false;
  int lt = xml.indexOf('<', gt + 1);
  if (lt < 0) return false;
  String v = xml.substring(gt + 1, lt);
  v.trim();
  if (v.length() == 0) return false;
  out = v.toFloat();
  return true;
}

inline void sourcesCcmPoll() {
  NetworkUDP &u = ccmRxSocket();
  int sz = u.parsePacket();
  if (sz <= 0) return;
  char buf[600];
  int n = u.read(buf, sizeof(buf) - 1);
  if (n <= 0) return;
  buf[n] = 0;
  String xml(buf);

  float v;
  if (ccmExtract(xml, g_cfg.co2_type, v) && v > 0) {
    g_co2_ppm = v; g_co2_ms = millis(); g_co2_src = "ccm";
  }
  if (ccmExtract(xml, g_cfg.temp_type, v)) {
    g_temp_c = v; g_temp_ms = millis();
  }
}

// ---- MQTT subscribe callback -----------------------------------------------
inline void onMqttMessage(char *topic, byte *payload, unsigned int len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;
  float value = doc["value"] | NAN;
  if (isnan(value)) return;

  if (co2Topic() == topic) {
    if (value > 0) { g_co2_ppm = value; g_co2_ms = millis(); g_co2_src = "mqtt"; }
  } else if (tempTopic() == topic) {
    g_temp_c = value; g_temp_ms = millis();
  }
}

inline void sourcesSubscribe() {
  agri::MQTT::mqtt.subscribe(co2Topic().c_str());
  if (g_cfg.temp_type[0]) agri::MQTT::mqtt.subscribe(tempTopic().c_str());
  Serial.printf("[SUB] %s / %s\n", co2Topic().c_str(), tempTopic().c_str());
}

inline void sourcesBegin() {
  co2Topic()  = String(g_cfg.src_prefix) + "/sensor/" + g_cfg.co2_type;
  tempTopic() = String(g_cfg.src_prefix) + "/sensor/" + g_cfg.temp_type;
  agri::MQTT::mqtt.setCallback(onMqttMessage);
  if (g_cfg.common.ccm_enabled) ccmRxSocket().begin(16520);
}

// ---- window position poll (read-only ArSprout status) ----------------------
inline bool winPollOne(uint16_t id, int &pct, String &mode) {
  if (id == 0) return false;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(3000);
  String url = String("http://") + g_cfg.arsprout_host + "/api/component/" + id;
  if (!http.begin(url)) return false;
  http.addHeader("Authorization", ARSPROUT_AUTH);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      pct  = doc["value"].as<int>();
      mode = String(doc["mode"] | "?");
      ok = true;
    }
  }
  http.end();
  return ok;
}

inline void sourcesWinPoll() {
  if (!g_cfg.win_gate) return;
  int best = -1; String mode = "?";
  int pct; String m;
  if (winPollOne(g_cfg.win_id1, pct, m)) { best = max(best, pct); mode = m; }
  if (winPollOne(g_cfg.win_id2, pct, m)) { best = max(best, pct); mode = m; }
  if (best >= 0) {
    g_win_pct = best;
    g_win_ms  = millis();
    strlcpy(g_win_mode, mode.c_str(), sizeof(g_win_mode));
  }
}

// Freshness helpers ----------------------------------------------------------
inline bool co2Fresh() {
  return g_co2_ms != 0 && !isnan(g_co2_ppm) &&
         (millis() - g_co2_ms) < (uint32_t)g_cfg.src_stale_s * 1000UL;
}
inline bool tempFresh() {
  return g_temp_ms != 0 && !isnan(g_temp_c) &&
         (millis() - g_temp_ms) < (uint32_t)g_cfg.src_stale_s * 1000UL;
}
inline bool winFresh() {
  return g_win_ms != 0 && g_win_pct >= 0 &&
         (millis() - g_win_ms) < (uint32_t)g_cfg.win_stale_s * 1000UL;
}
