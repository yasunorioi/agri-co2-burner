// agri-co2-burner — M5 AtomHub Switch (K042 / ATOM Lite) CO2-dosing controller
// for a Shizuoka Seiki CG-1000 kerosene CO2 generator.
//
// WiFi node (the agri-* WiFi exception — no W5500, core's agri::Network unused).
// Subscribes to CO2/temp over agriha MQTT (CCM 16520 fallback), polls ArSprout
// side-window position, and drives RELAY1 (G22) to enable the burner while
// keeping CO2 near target — bounded by a hard-max, a duty limit, min on/off
// dwell, a daytime schedule, and a window-open gate. Autonomous: it keeps
// running if the Pi4/yasu-hp master dies, and publishes its own relay state
// back to MQTT so the master/DSL can observe (and later override).

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>
#include <FastLED.h>
#include <AgriNode.h>

#include "config.h"
#include "sources.h"
#include "control.h"
#include "webui_hooks.h"

const char *FW_NAME     = "agri-co2-burner";
const char *FW_VERSION  = "0.1.0";
const char *FW_REPO     = "yasunorioi/agri-co2-burner";
const char *FW_BIN_NAME = "agri-co2-burner.bin";

// ---- globals (declared extern in the headers) ------------------------------
AppConfig g_cfg;

float    g_co2_ppm   = NAN;
uint32_t g_co2_ms    = 0;
const char *g_co2_src = "-";
float    g_temp_c    = NAN;
uint32_t g_temp_ms   = 0;

int      g_win_pct   = -1;
uint32_t g_win_ms    = 0;
char     g_win_mode[12] = "?";

bool        g_relay     = false;
uint32_t    g_relay_ms  = 0;
const char *g_reason    = "boot";
uint32_t    g_duty_win_start_ms = 0;
uint32_t    g_duty_on_ms        = 0;
uint32_t    g_duty_last_ms      = 0;

static bool g_wm_should_save = false;   // set by WiFiManager save callback

// ---- WiFi bring-up ---------------------------------------------------------
static bool netConnected() { return WiFi.status() == WL_CONNECTED; }

// WiFiManager owns the WiFi credentials + provisioning portal. We piggy-back
// the MQTT host/port/prefix as custom fields so one portal visit configures
// everything. On a normal boot (creds saved) the portal never shows and we
// keep whatever MQTT config is already in NVS. Autonomy: the portal has a
// timeout so a temporarily-unreachable AP never blocks the controller forever;
// after that loop() keeps retrying WiFi while the control loop runs (fail-safe
// OFF while offline). Hold the button (G39) at boot to force re-provisioning.
static void wifiBegin() {
  WiFiManager wm;
  wm.setHostname(g_cfg.common.hostname);
  wm.setConfigPortalTimeout(300);
  wm.setSaveConfigCallback([](){ g_wm_should_save = true; });

  char portStr[8];
  snprintf(portStr, sizeof(portStr), "%u", g_cfg.common.mqtt_port);
  WiFiManagerParameter p_host("mhost", "MQTT host (IP)", g_cfg.common.mqtt_host, 63);
  WiFiManagerParameter p_port("mport", "MQTT port", portStr, 7);
  WiFiManagerParameter p_pfx ("mpfx",  "MQTT prefix (e.g. agriha/3)",
                              g_cfg.common.mqtt_topic_prefix, 63);
  wm.addParameter(&p_host);
  wm.addParameter(&p_port);
  wm.addParameter(&p_pfx);

  bool force = (digitalRead(39) == LOW);   // button held at boot
  bool ok = force ? wm.startConfigPortal("agri-co2-setup")
                  : wm.autoConnect("agri-co2-setup");

  if (g_wm_should_save) {
    strlcpy(g_cfg.common.mqtt_host, p_host.getValue(), sizeof(g_cfg.common.mqtt_host));
    int port = atoi(p_port.getValue());
    if (port > 0 && port < 65536) g_cfg.common.mqtt_port = (uint16_t)port;
    strlcpy(g_cfg.common.mqtt_topic_prefix, p_pfx.getValue(),
            sizeof(g_cfg.common.mqtt_topic_prefix));
    strlcpy(g_cfg.src_prefix, p_pfx.getValue(), sizeof(g_cfg.src_prefix));
    saveConfig();
    Serial.printf("[WM] saved MQTT host=%s port=%u prefix=%s\n",
                  g_cfg.common.mqtt_host, g_cfg.common.mqtt_port,
                  g_cfg.common.mqtt_topic_prefix);
  }

  if (ok) Serial.printf("[WiFi] connected, IP %s\n", WiFi.localIP().toString().c_str());
  else    Serial.println("[WiFi] not connected (portal timeout) — running offline, will retry");
}

// ---- own-state publish -----------------------------------------------------
static bool publishBurnerState() {
  char topic[96];
  snprintf(topic, sizeof(topic), "%s/actuator/CO2Burner",
           g_cfg.common.mqtt_topic_prefix);
  char payload[80];
  snprintf(payload, sizeof(payload), "{\"value\":%d,\"unit\":\"bool\",\"ts\":%ld}",
           g_relay ? 1 : 0, nowEpoch());
  return agri::MQTT::mqtt.publish(topic, (const uint8_t *)payload,
                                  strlen(payload), true /*retained*/);
}

// ---- button (ATOM Lite G39, active LOW): cycle AUTO→FORCE_OFF→FORCE_ON ------
static void buttonPoll() {
  static bool     last = HIGH;
  static uint32_t lastChg = 0;
  bool b = digitalRead(39);
  if (b != last && millis() - lastChg > 60) {
    lastChg = millis();
    if (b == LOW) {   // press edge
      g_cfg.mode = (g_cfg.mode == MODE_AUTO)      ? MODE_FORCE_OFF
                 : (g_cfg.mode == MODE_FORCE_OFF) ? MODE_FORCE_ON
                                                  : MODE_AUTO;
      Serial.printf("[BTN] mode → %s\n", modeName(g_cfg.mode));
      saveConfig();
    }
    last = b;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n=== %s v%s ===\n", FW_NAME, FW_VERSION);

  agri::Led::begin();
  pinMode(39, INPUT);
  loadConfig();
  controlBegin();     // relay OFF, duty window reset

  wifiBegin();

  // JST for the schedule (localtime_r); time(nullptr) stays UTC for {…,ts}.
  configTime(9 * 3600, 0, "pool.ntp.org", "time.google.com");

  agri::MQTT::begin();
  sourcesBegin();     // topic strings + MQTT callback + optional CCM socket

  agri::WebHooks hooks;
  hooks.nodeTitle              = [](){ return FW_NAME; };
  hooks.renderDashboardSensors = renderDashboard;
  hooks.renderConfigSensorRows = renderConfigRows;
  hooks.applyConfigSensorForm  = applyConfigRows;
  hooks.addStatusFields        = addStatusFields;
  hooks.saveConfig             = [](){ saveConfig(); };
  agri::WebUI::begin(g_cfg.common, hooks, FW_NAME, FW_VERSION);

  agri::mdnsBegin(g_cfg.common.hostname);
  agri::otaBegin(g_cfg.common.hostname);
  agri::OTA::begin(FW_REPO, FW_BIN_NAME, FW_VERSION);
  if (netConnected()) agri::OTA::checkLatest();

  Serial.println("[BOOT] ready");
}

void loop() {
  uint32_t now = millis();

  agri::otaHandle();
  agri::OTA::poll();
  agri::WebUI::handle(netConnected(), netConnected());
  buttonPoll();

  // STA retry if we dropped (WiFiManager saved the creds in esp_wifi NVS)
  if (!netConnected()) {
    static uint32_t lastTry = 0;
    if (now - lastTry > 10000) { lastTry = now; WiFi.reconnect(); }
  }

  // MQTT connect + subscribe-on-(re)connect
  static bool wasConnected = false;
  if (netConnected() && agri::MQTT::hasHost(g_cfg.common)) {
    if (!agri::MQTT::connected()) {
      wasConnected = false;
      static uint32_t lastTry = 0;
      if (now - lastTry > 5000) { lastTry = now; agri::MQTT::reconnect(g_cfg.common); }
    } else {
      if (!wasConnected) { wasConnected = true; sourcesSubscribe(); publishBurnerState(); }
      agri::MQTT::loop();
    }
  }

  // CCM fallback receive (drain any pending packets)
  if (g_cfg.common.ccm_enabled && netConnected()) sourcesCcmPoll();

  // window position poll
  static uint32_t lastWin = 0;
  if (netConnected() &&
      now - lastWin >= (uint32_t)g_cfg.win_poll_s * 1000UL) {
    lastWin = now;
    sourcesWinPoll();
  }

  // control loop (1 Hz) + publish own state on change or on interval
  static uint32_t lastCtl = 0;
  if (now - lastCtl >= 1000) {
    lastCtl = now;
    bool prevRelay = g_relay;
    controlTick(now);
    static uint32_t lastPub = 0;
    uint32_t interval = (uint32_t)g_cfg.common.mqtt_interval_s * 1000UL;
    if (agri::MQTT::connected() &&
        (g_relay != prevRelay || now - lastPub >= interval)) {
      lastPub = now;
      if (publishBurnerState()) agri::Led::flashPublish();
    }
  }

  // LED: net/mqtt state, repainted each loop; ON overrides to burner-orange.
  agri::LedState desired;
  if (!netConnected())                                                   desired = agri::LED_NO_LINK;
  else if (agri::MQTT::hasHost(g_cfg.common) && !agri::MQTT::connected()) desired = agri::LED_NO_MQTT;
  else                                                                   desired = agri::LED_OK;
  agri::Led::set(desired);
  agri::Led::apply();
  if (g_relay) { agri::Led::pixel[0] = CRGB(90, 35, 0); FastLED.show(); }

  static uint32_t lastStatus = 0;
  if (now - lastStatus >= 30000) {
    lastStatus = now;
    Serial.printf("[STATUS] wifi=%d mqtt=%d relay=%d mode=%s CO2=%.0f(%s,%s) win=%d duty=%u%% (%s)\n",
                  netConnected(), agri::MQTT::connected(), g_relay,
                  modeName(g_cfg.mode), g_co2_ppm, g_co2_src,
                  co2Fresh() ? "fresh" : "stale", g_win_pct, dutyPctNow(),
                  g_reason ? g_reason : "-");
  }

  delay(20);
}
