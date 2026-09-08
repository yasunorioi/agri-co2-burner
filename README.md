# agri-co2-burner

CO₂-dosing controller for a **Shizuoka Seiki CG-1000** kerosene CO₂ generator,
built on the **M5Stack AtomHub Switch (K042)** — an ATOM Lite (ESP32) with two
AC 250 V / 10 A relays. Part of the `agri-*` family, reusing
[`agri-node-poe-core`](https://github.com/yasunorioi/agri-node-poe-core) for
config / MQTT / WebUI / OTA / LED.

Unlike the PoE sensor nodes, this is a **WiFi** node (the AtomHub Switch is
AC-DC powered, no W5500) and an **actuator** rather than a sensor. It reads
CO₂/temperature that *other* nodes publish, decides autonomously, and drives
the burner.

## What it does

- Keeps greenhouse CO₂ near a target (default **500 ppm**) with hysteresis
  (default ON ≤ 450, OFF ≥ 520 ppm).
- **Autonomous**: the decision lives on the node, so it keeps dosing if the
  Pi4 / yasu-hp master is down. It publishes its own relay state to MQTT so the
  master / DSL can observe and later override.
- Avoids wasting gas when the **side windows are open** (another system opens
  them): gated by both a **daytime schedule** *and* the **actual window
  position** polled from ArSprout.

### Safety / spec guards (always enforced, even in FORCE ON)

| Guard | Default | Why |
|---|---|---|
| Hard-max CO₂ → force OFF | 1000 ppm | CG-1000 spec: keep ≤ 1000 ppm |
| Duty limit | ≤ 50 % / hour | CG-1000 spec: ≤ 50 % duty per hour |
| Fail-safe on stale sensor / WiFi loss | 180 s | never run a burner blind |
| Min ON / Min OFF dwell | 300 s / 300 s | anti short-cycle of the burner |

## Inputs

| Input | Source | Notes |
|---|---|---|
| CO₂ (primary) | MQTT `<src_prefix>/sensor/InAirCO2` | agriha `{value,unit,ts}` (e.g. from [`agri-env-poe`](https://github.com/yasunorioi/agri-env-poe) SCD41) |
| CO₂ (fallback) | UECS-CCM UDP :16520 `InAirCO2.cMC` | e.g. ArSprout's own broadcast; enable with core's `CCM enabled` |
| Temp (optional gate) | MQTT `<src_prefix>/sensor/InAirTemp` | dose only within `[min,max]` when enabled |
| Side window | ArSprout `GET /api/component/<id>` → `{value,mode}` | **read-only** status endpoint (not `operate`); ids 64/65 = h3 東/西 |

## Wiring

```
CG-1000 external switch/contact ── RELAY1 (G22, active HIGH) on AtomHub Switch
                                   RELAY2 (G19) = spare
LED  = G27 (WS2812)   Button = G39 (cycle AUTO / FORCE OFF / FORCE ON)
Spare: PORT.A I2C G26/G32, RS485 — for a future local sensor (fully-offline dosing)
```

LED: blue boot · red no-WiFi · magenta AP-provisioning · yellow no-MQTT ·
green OK · **orange = relay ON (burner enabled)** · white publish-flash.

## First run

1. Flash over USB-C (`pio run -t upload`).
2. No WiFi creds yet → node starts SoftAP **`agri-co2-setup`** with a catch-all
   DNS + captive-portal redirect. Join it from a phone and the sign-in browser
   should pop straight to the config page (or open `http://192.168.4.1/config`
   manually). Set SSID/passphrase + MQTT host + source prefix, Save, and reboot.
   (Captive redirect needs `agri-node-poe-core` with the `WebUI::captive` flag.)
3. Thereafter it joins your WiFi as `agri-co2-01.local`; tune setpoints /
   schedule / window gate on `/config`. Later flashes via ArduinoOTA or `/ota`.

## ⚠ Confirm before trusting in production

- **House / prefix**: defaults assume **h3** (`agriha/3`, window ids 64/65,
  ArSprout `192.168.1.81`). Change on `/config` if this lives elsewhere.
- **Schedule default 08:00–15:00** is a placeholder — set it to the hours
  *before* the side windows open for the crop.
- **Temperature gate**: OFF by default. Decide whether temp should be a gate
  (dose only within a range) or, for a heat-producing burner, an *additional*
  ON trigger on cold mornings — the latter is not implemented yet.
- The CO₂-source house prefix (`src_prefix`) may differ from this node's own
  publish prefix; both are editable.

## Config keys

All tunables are on the web `/config` page and persisted in NVS (namespace
`co2brn`). `FORCE ON` is downgraded to `AUTO` on reboot so an unattended
restart never powers the burner on.

## Flashing & serial notes

- **USB-serial chip**: this AtomHub Switch enumerates as an **FTDI FT232**
  (`0403:6001`), *not* the CH9102F of a bare ATOM Lite. Its programming path is
  wired for auto reset (EN/IO0 via DTR/RTS), so `pio run -t upload` flashes and
  hard-resets normally. `upload_speed = 115200` works as-is.
- **First boot after flash** showed a single `TG1WDT_SYS_RESET` between the
  banner and setup completing, then booted clean and stayed up — consistent
  with radio-init inrush on a marginal supply. Power it from a supply with
  headroom; if the WDT reset recurs at the final install, suspect brownout.
- **Headless serial**: `pio device monitor` crashes without a TTY
  (`Console()`). Read the port directly instead, e.g. pyserial with
  `dtr=False; rts=False` to monitor *without* resetting the board (pulse RTS
  once if you want to capture a fresh boot).
- **Verified on device**: boots into SoftAP `agri-co2-setup` (captive → /config)
  as `agri-co2-01.local`; with no CO2 data the control loop holds the relay
  `OFF` (`CO2 stale → fail-safe OFF`).

## License

0BSD — copy and adapt freely.
