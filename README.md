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
| CO₂ (primary) | MQTT `<src_prefix>/<src_category>/InAirCO2` | agriha `{value,unit,ts}` (e.g. from [`agri-env-poe`](https://github.com/yasunorioi/agri-env-poe) SCD41) |
| CO₂ (fallback) | UECS-CCM UDP :16520 `InAirCO2.cMC` | e.g. ArSprout's own broadcast; enable with core's `CCM enabled` |
| Temp (optional gate) | MQTT `<src_prefix>/<src_category>/InAirTemp` | dose only within `[min,max]` when enabled |
| Side window | ArSprout `GET /api/component/<id>` → `{value,mode}` | **read-only** status endpoint (not `operate`) |

### `src_category` — なぜ `sensor` 固定ではないのか

購読先は `<src_prefix>/<src_category>/<type>`。ハウスによって値の出口が違う。

| | 使う category | 実際の出所 |
|---|---|---|
| native な agriha ノードがある | `sensor` | ノードが直接 publish |
| ArSprout/UECS ノードしかない | `sensor_ccm` | CCM ブリッジが変換して publish |

house2 は後者で、CO₂ は `agriha/2/sensor_ccm/InAirCO2` にしか出てこない。
v0.1.0 は `sensor` 固定だったため **house2 のノードは購読先が存在せず永久に stale** だった。

### 側窓の component id

**別ハウスの窓を指しても症状が出ない**（ゲートは fresh のまま値だけ間違う）ので、
`GET /api/component` の `CcmRegion`/`CcmOrder` と突き合わせて確認すること。
すべて `.81` が持つ:

| CCM | id | 名称 | ハウス |
|---|---|---|---|
| region 71 order 1 / 2 | **32 / 33** | 側窓東2 / 側窓西2 | house2 |
| region 72 order 1 / 2 | **64 / 65** | 側窓東3 / 側窓西3 | house3 |

house1 の窓は region 61 で、`.81` ではなく旧 ArSprout `.71` 側。

## Wiring

```
CG-1000 external switch/contact ── RELAY1 (G22, active HIGH) on AtomHub Switch
                                   RELAY2 (G19) = spare
LED  = G27 (WS2812)   Button = G39 (cycle AUTO / FORCE OFF / FORCE ON)
Spare: PORT.A I2C G26/G32, RS485 — for a future local sensor (fully-offline dosing)
```

LED: blue boot / portal · red no-WiFi · yellow no-MQTT · green OK ·
**orange = relay ON (burner enabled)** · white publish-flash.

## First run — WiFi provisioning

WiFi credentials are handled by [WiFiManager](https://github.com/tzapu/WiFiManager);
the MQTT host / port / prefix ride along as custom fields so one portal visit
configures everything.

1. Flash over USB-C (`pio run -t upload`).
2. No WiFi saved → the node starts an open AP **`agri-co2-setup`** and its
   captive portal. Join it from a phone; the portal opens automatically.
   **Configure WiFi** → pick your SSID from the scan + enter the passphrase,
   and fill the custom fields (**MQTT host** IP, **MQTT port** `1883`,
   **MQTT prefix** e.g. `agriha/3`). Save.
3. The node connects, persists the MQTT fields to NVS (prefix is also copied to
   `src_prefix`), and joins your WiFi as `agri-co2-01.local`. Tune setpoints /
   schedule / window gate — and set/change the MQTT host later — on `/config`.
   Later flashes via ArduinoOTA or `/ota`.
4. **Re-provision**: hold the button (G39) while powering on to force the
   portal back up. WiFiManager's portal has a 300 s timeout so a temporarily
   unreachable AP never blocks the controller — it runs offline (relay held
   OFF by fail-safe) and `loop()` keeps retrying WiFi.

> Note: core's `WebUI::captive` flag (added for the earlier hand-rolled AP) is
> no longer used here — WiFiManager owns provisioning — but is harmless.

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
- **Verified on device**: WiFiManager portal → joined WiFi as
  `agri-co2-01.local`, reachable over HTTP; `/api/status` shows `wifi_ip` +
  RSSI, and with no CO2 data / no MQTT host the control loop holds the relay
  `OFF` (`CO2 stale → fail-safe OFF`). (`ip: 0.0.0.0` in status is cosmetic —
  core reads `ETH.localIP()`; the real address is `wifi_ip`.)

## License

0BSD — copy and adapt freely.
