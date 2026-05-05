# 🌱 TerraFirm
### ESP32 Greenhouse Monitoring & Control

**Because your plants deserve better than a prayer and a watering can.**

No cloud bloat. No missed waterings. No overcooked crops. Just a soil reading every 2 seconds, three layers of noise filtering, and exactly zero tolerance for a flooded pot.

---

## 🤔 The Problem

Your greenhouse doesn't care that you're asleep.

At 2 AM, the soil goes dry. At noon, temperatures spike past 30°C. Your fan just sits there. And your plants — quietly, without complaint, with absolute zero drama — start dying.

Manual watering is vibes-based engineering. Timer systems flood or starve because they ignore actual soil conditions. And raw ADC readings without filtering? Your pump triggers every 4 seconds because a relay click looked like a drought to a microcontroller that had never heard of a median filter.

**TerraFirm manages your greenhouse the way a paranoid botanist would** — suspicious of every ADC spike, cross-referencing every reading against the last 30 seconds of history, and physically incapable of making a control decision without running the signal through three processing layers first.

---

## 📌 What This Is

An end-to-end ESP32 firmware for automated greenhouse management, built from actual hardware failures and a deeply personal relationship with the IN4007 diode:

- **Real-time sensing** — temperature, humidity, and soil moisture read every 2 seconds
- **Three-layer signal pipeline** — median filter → EMA smoothing → 30s circular buffer
- **Autonomous control** — pump and fan with hysteresis bands, 30s safety timeout, 60s cooldown
- **Dual cloud** — Blynk for live remote control, ThingSpeak for long-term trend logging
- **Offline capable** — full local control + LCD continues working with no WiFi
- **Crash-proof** — hardware watchdog, exponential WiFi backoff, non-blocking `loop()`

Single `.ino` file. Flash and forget. Your plants will not forget.

---

## 🏆 What the Filtering Actually Does

Before filtering: soil readings jumping 0% → 97% → 12% → 88%. Pump triggering and cancelling every 4 seconds. Completely unusable.

After the three-layer pipeline:

| Stage | What it does | Result |
|---|---|---|
| Raw ADC | 12-bit, 0–4095 | Swings ±400 counts per relay click |
| Median filter (5 samples) | Removes single-sample EMI spikes entirely | 1 spike in 5 readings → gone before EMA sees it |
| EMA (α = 0.30) | Tracks slow trends, ignores fast noise | Values drift ≤2% per reading |
| 30s circular buffer | Averages 15 samples before cloud upload | ThingSpeak never sees a raw ADC value |

The pump now triggers when soil is actually dry. The fan now triggers when it's actually hot. Readings move 1–2% per cycle instead of teleporting.

> **The counterintuitive finding:** EMA alone isn't enough. α=0.30 still absorbs 30% of a 400-count spike — enough to misreport moisture and suppress the next pump cycle mid-run. You need the median filter *first* to kill the outlier before EMA ever sees it. Order matters.

---

## 🔒 Safety System — Why Every Layer Exists

| Protection | What it prevents | What happens without it |
|---|---|---|
| `RELAY_OFF` as first line of `setup()` | Boot pulse on active-LOW relay | Pump fires on every restart. Undetectable in testing |
| 10s startup hold | False trigger on cold sensor | DHT11 reads garbage for the first few seconds |
| 30s pump max runtime | Flood if soil sensor fails mid-run | Pump runs until someone physically notices |
| 60s pump cooldown | Motor thermal damage | Motor overheats on repeated short cycles |
| 2°C fan hysteresis | Fan rapid-cycling at threshold boundary | Fan toggling on/off every few seconds at 30°C |
| DHT fault tolerance (5 failures) | Single bad read killing control decisions | Transient glitch disables fan at peak temperature |
| Watchdog (10s timeout) | `loop()` blocking on WiFi call | System hangs with relays frozen in last-known state |
| Exponential WiFi backoff | Reconnect spam during outages | Aggressive retries starve the loop; watchdog fires |
| IN4007 diodes | Flyback spike from motor/pump | 50–100V reverse spike onto 5V rail, into ESP32 and LCD |
| 100µF capacitor | Inrush voltage dip on relay energise | Brownout reset mid-cycle |

Every row in that table is a failure mode that was discovered, not predicted.

---

## 💀 Three Hardware Mistakes That Happened In Real Life

This project did not begin with a 5V 2A adapter and flyback diodes. It began with two 3.7V lithium-ion batteries and several mistaken assumptions. Here is the complete and unredacted failure log.

---

### Mistake 1 — Two 3.7V Li-Ion Batteries Powering Everything

**What happened:** Two li-ion cells. Motor running. Pump running. ESP32 running. No WiFi — just the hardware, standalone. Worked fine. Then the ESP32 froze. The LCD froze. Serial Monitor on the laptop locked up entirely. No warning.

**Why:** Li-ion cells are not 3.7V. That's the nominal resting voltage. Fully charged they're 4.2V each — 8.4V in series at peak, sagging to ~6V as they drain, unregulated the entire time. Add two inductive loads switching on and off with zero flyback protection, zero decoupling, and a microcontroller that was not consulted about any of this. The ESP32 survived on internal protection. The LCD and USB-serial bridge were less lucky.

**Fix:** Binned the batteries. 5V 2A regulated wall adapter. Every symptom from this phase disappeared within one boot.

> "It worked for a while" is the most dangerous phrase in hardware. Inductive damage is cumulative. The system runs fine, then one relay click sends a spike that crosses a threshold something was quietly approaching, and the failure feels completely random. It is not random. It was always going to happen.

---

### Mistake 2 — Relay Switching Was Crashing the LCD and Freezing Serial Output

**What happened:** Proper supply in. Motor and pump running correctly. But every relay switch — on or off — made the LCD glitch or go blank. Serial Monitor output stalled mid-line. Spent real time staring at firmware that was not the problem.

**Why:** Motors and pumps are inductive loads. Cut power to an inductor and its magnetic field collapses, generating a reverse voltage spike — potentially 50–100V for a few microseconds. Without a flyback diode, that spike travels back through the relay coil onto the 5V rail and into everything sharing it: the ESP32, the LCD, and the USB-to-serial bridge connected to the laptop. The Serial Monitor freezing was that bridge chip absorbing an 80V spike through the USB cable. The cable did not ask for this responsibility.

**Fix:** One IN4007 diode across each motor/pump load — cathode (stripe) toward positive. The spike now has a safe path to loop back and dissipate instead of escaping onto the rail. 100µF electrolytic capacitor across the 5V rail handles the voltage dip when relays first energise.

```
Motor / pump terminals:
  (+) ──┬─── [load] ───┬── (−)
        │              │
        └─────▶|───────┘
               IN4007
          cathode toward (+)
```

> Every project with a motor eventually teaches you about flyback diodes. The lesson is always the same: something freezes that has no business freezing, you spend an hour convinced it's a software bug, and then you Google "relay crashes microcontroller" and immediately feel seen by 40 StackOverflow threads from 2009.

---

### Mistake 3 — Raw ADC: 0%, 97%, 12%, 88%, Repeat

**What happened:** Hardware stable. Blynk dashboard showing soil moisture as a modern art installation — jumping from 8% to 96% to 31% to 87% with no physical change in the soil. Pump triggered, saw 90% moisture 2 seconds later, immediately cancelled. Then triggered again. The soil had not moved. The readings were hallucinating.

**Why:** The ESP32's ADC is a 12-bit converter on the same board as a WiFi radio and two relay coils. Any electromagnetic event — relay click, WiFi burst, motor commutation noise — couples into the ADC line and throws the reading by hundreds of counts per sample. A 400-count spike on a 4095-count scale is 10% of fake moisture from nothing. The circuit sneezed. The firmware believed it.

**Fix:** Three filtering layers in sequence, because each one solves a different problem:

```
Raw ADC — swinging wildly on every relay click
      ↓
Median filter — 5 samples, 200µs apart, take the middle value
      One EMI spike in five samples? Gone. Doesn't reach EMA.
      ↓
EMA — α = 0.30
      new = 0.30 × filtered + 0.70 × previous
      Tracks genuine slow soil changes. Ignores anything under 2 seconds.
      ↓
30s circular buffer → averaged → ThingSpeak
      What the cloud receives. Noise is invisible at this point.
```

Before: 0% → 97% → 12% — soil unchanged, pump cycling constantly.  
After: 42% → 43% → 43% — soil unchanged, pump correctly idle.

> Raw ADC on an ESP32 running WiFi and relay loads is measuring soil moisture *plus* WiFi radio interference *plus* relay switching transients *plus* motor EMI *plus* thermal drift. Filtering is not optional. It is the actual measurement.

---

## ⚡ Power

**5V 2A regulated wall adapter. That's the whole section.**

The ESP32 dev board accepts 5V via USB or the `Vin` pin. Its onboard AMS1117 regulator handles the step-down to 3.3V internally — the chip and all GPIO logic run at 3.3V, but the board input is 5V. The relay module VCC needs 5V. The LCD needs 5V. One supply rail, one adapter, no batteries.

> The 100µF cap and IN4007 diodes are not "nice to have." They are circuit protection that was added after failures, not before. See Mistake 1 and Mistake 2 above.

---

## 🔌 Hardware

| Component | Model / Spec | Notes |
|---|---|---|
| Microcontroller | ESP32 / ESP32-S3 | 5V input via USB or Vin, 3.3V GPIO logic |
| Temp + Humidity | DHT11 | 10kΩ pull-up resistor on DATA pin — required |
| Soil Sensor | Capacitive analog out | Calibrate dry/wet ADC before first use |
| Relays | 2× SRD-05VDC-SL-C | Active-LOW. VCC = 5V. IN logic = 3.3V compatible |
| Flyback protection | 2× IN4007 diodes | Across each motor/pump load. Non-negotiable |
| Rail stabilisation | 100µF electrolytic | Across 5V/GND near relay VCC. Positive leg to 5V |
| Display | I2C LCD 16×2 | Auto-detected at 0x27 or 0x3F on boot |
| Status LED | Any 5mm LED | 330Ω series to GND — mirrors fan state |
| Power | 5V 2A regulated adapter | The whole system. No alternatives |

### Pin Reference

```
ESP32 GPIO  →  Component
──────────────────────────────────────────────────
GPIO 4   →  DHT11 DATA       (+ 10kΩ pull-up to 3.3V)
GPIO 32  →  Soil sensor AOUT (ADC1_CH4)
GPIO 26  →  Fan relay IN1    (active-LOW: LOW = fan ON)
GPIO 27  →  Pump relay IN2   (active-LOW: LOW = pump ON)
GPIO 25  →  Status LED       (+ 330Ω to GND, mirrors fan)
GPIO 21  →  LCD SDA
GPIO 22  →  LCD SCL
3.3V     →  DHT11 VCC, Soil sensor VCC
5V       →  Relay VCC, LCD VCC, 100µF cap (+)
GND      →  Everything. One common ground. No exceptions.
```

---

## ⚙️ How It Works

```
Power on
  ↓
RELAY_OFF on all pins  ← first, before anything else executes
  ↓
I2C scan → LCD auto-detected (0x27 or 0x3F)
  ↓
DHT11 init → WiFi (8s timeout) → Blynk.config() → ThingSpeak.begin()
           → WiFi fail → offline mode (local control still runs fully)
  ↓
Watchdog armed: 10s timeout, panic reset on hang
  ↓
10s startup hold  ← sensors stabilise, no control decisions made
  ↓
──────────── Main loop (non-blocking, all time-sliced) ────────────
  │
  ├─ Every 2s:  Soil ADC → medianADC(5) → EMA → 0–100%
  ├─ Every 2s:  DHT11 → temp + humidity → EMA
  ├─ Every 2s:  Snapshot → circular buffer (15 × 2s = 30s)
  ├─ Every 2s:  Pump [ON <30% / OFF >60% / 30s hard cap / 60s cooldown]
  ├─ Every 2s:  Fan  [ON ≥30°C / OFF ≤28°C / 2°C hysteresis]
  ├─ Every 2s:  LCD update
  ├─ Every 2s:  Serial status line
  ├─ Every 30s: Blynk push    (V0/V1/V3/V4/V5)
  ├─ Every 30s: ThingSpeak    (Fields 1–6, buffer averaged)
  ├─ Every 10s: WiFi health   (backoff: 10→20→40→60s cap)
  └─ Every loop: esp_task_wdt_reset()
```

---

## 🛠️ Tunable Thresholds

### Pump — Soil Moisture

| Parameter | Default | Effect of changing it |
|---|---|---|
| `PUMP_ON_MOISTURE` | 30% | Lower = water more aggressively. Raise for drought-tolerant plants |
| `PUMP_OFF_MOISTURE` | 60% | Controls overshoot. Higher = wetter target |
| `PUMP_MAX_ON_MS` | 30,000ms | Safety cap. Lower for small pots, raise for large beds |
| `PUMP_COOLDOWN_MS` | 60,000ms | Motor rest period. Never go below 30s |

### Fan — Temperature

| Parameter | Default | Effect of changing it |
|---|---|---|
| `FAN_ON_TEMP` | 30.0°C | Lower for heat-sensitive plants |
| `FAN_OFF_TEMP` | 28.0°C | Gap must be ≥1°C or fan rapid-cycles |

### Timing

| Parameter | Default | Why this value |
|---|---|---|
| `STARTUP_HOLD_MS` | 10,000ms | DHT11 stabilisation on power-up |
| `SENSOR_INTERVAL_MS` | 2,000ms | DHT11 max rate is 1Hz. 2s adds margin |
| `BLYNK_SEND_INTERVAL_MS` | 30,000ms | Within Blynk free tier limits |
| `THINGSPEAK_INTERVAL_MS` | 30,000ms | Matches 15-sample buffer exactly |
| `WDT_TIMEOUT_S` | 10s | Must exceed longest blocking operation |

---

## 📡 Cloud Integration

### Blynk — Virtual Pin Map

| Virtual Pin | Data | Notes |
|---|---|---|
| V0 | Temperature (°C) | EMA-smoothed. Only sent when valid |
| V1 | Humidity (%) | EMA-smoothed. Only sent when valid |
| V3 | Pump state (0/1) | Pushed immediately on every state change |
| V4 | Fan state (0/1) | Pushed immediately on every state change |
| V5 | Soil moisture (%) | EMA-smoothed |

Blynk uses `Blynk.config()` + `Blynk.connect()` — WiFi connects first independently, then Blynk attaches. This means local control runs even when Blynk servers are unreachable. Pump and fan state changes are pushed to Blynk immediately via `virtualWrite`, not just on the 30s schedule — so the app reflects reality within milliseconds of a relay switching.

### ThingSpeak — Channel Fields

| Field | Data | What it tells you |
|---|---|---|
| Field 1 | 30s avg temperature | Trend, not a snapshot |
| Field 2 | 30s avg humidity | Trend, not a snapshot |
| Field 3 | 30s avg soil moisture | Trend, not a snapshot |
| Field 4 | Pump duty cycle (% ON) | Irrigation load — see below |
| Field 5 | Fan duty cycle (% ON) | Cumulative heat stress |
| Field 6 | Sample count | Buffer fill at upload time |

**`pumpDuty` is the most diagnostic field.** A soil reading can lie — bad calibration, sensor drift, one ADC spike that slipped through. But a pump running at 80% duty cycle while `avgSoil` stays flat is physically impossible unless something is wrong with the irrigation: a clog, a split line, an empty reservoir, or a pump spinning but not moving water. The duty cycle doesn't lie even when the sensor does.

TerraFirm uploads immediately on first boot via a `firstSend` flag, without waiting the full 30s. After each successful upload, the circular buffer resets completely — the next ThingSpeak entry reflects only fresh data.

---

## 📺 LCD Display

```
Row 0:  S: 42%  T: 26C
Row 1:  H: 61%  P:OFF F:OFF C
```

The last character on Row 1 is the cloud status indicator:

| Character | Meaning |
|---|---|
| `C` | Both Blynk and ThingSpeak connected |
| `B` | Blynk connected only |
| `T` | ThingSpeak connected only |
| `-` | Offline — local control still running |

During the startup hold, Row 1 shows a countdown: `Hold:  9s`

---

## 🖥️ Serial Monitor

Open at **115200 baud**. Boot sequence:

```
=== TerraFirm v4.4 ===
[GPIO] Relays OFF
[I2C]  Device at 0x27
[LCD]  Ready at 0x27
[WiFi] Connected: 192.168.1.105
[ThingSpeak] Ready
[WDT]  Armed: 10s
[BOOT] Hold: 10s | Buffer: 15 samples
[HOLD] 9s — Soil:38% T:N/A
[HOLD] 7s — Soil:39% T:26.1
[BOOT] Control active
```

Every 2s during normal operation:

```
T:26.1 H:61.4 S:42%(R:2241) P:OFF F:OFF | V:THS | WiFi:OK(10s) | Blynk:OK | TS:OK | Buf:7/15
```

`R:xxxx` is the raw ADC value before any filtering — the most useful number during soil sensor calibration. Read it in dry air → set `SOIL_ADC_DRY`. Read it submerged in water → set `SOIL_ADC_WET`.

`V:THS` shows sensor validity flags: `T` = temperature valid, `H` = humidity valid, `S` = soil valid. Any `-` means that sensor has failed 5 consecutive reads.

---

## 🚀 Quickstart

### 1. Install Libraries

```
BlynkSimpleEsp32    >= 1.3.2
LiquidCrystal_I2C   >= 1.1.2
DHT sensor library  >= 1.4.4  (Adafruit)
ThingSpeak          >= 2.0.0
esp_task_wdt        (ESP32 Arduino Core >= 2.0.0, built-in)
```

Board: `esp32:esp32:esp32s3` or `esp32:esp32:esp32`

### 2. Set Credentials

At the top of `TerraFirm.ino`:

```cpp
// Blynk — Console → your device → Device Info
#define BLYNK_TEMPLATE_ID    "YOUR_TEMPLATE_ID"
#define BLYNK_TEMPLATE_NAME  "Garden Controller"
#define BLYNK_AUTH_TOKEN     "YOUR_AUTH_TOKEN"

// ThingSpeak — thingspeak.com → Channels → API Keys
#define THINGSPEAK_CHANNEL_ID    0000000
#define THINGSPEAK_WRITE_API_KEY "XXXXXXXXXXXXXXXX"

#define WIFI_SSID     "YourNetwork"
#define WIFI_PASSWORD "YourPassword"
```

### 3. Calibrate the Soil Sensor

Every capacitive sensor has different ADC values. This takes 2 minutes and determines whether your 30% threshold means "needs water" or "bone dry" or "already flooding":

**Step 1:** Hold sensor in open air → watch Serial Monitor → note the `R:xxxx` value:
```cpp
#define SOIL_ADC_DRY  4095  // replace with your value
```
**Step 2:** Submerge sensor tip in water → note the `R:xxxx` value:
```cpp
#define SOIL_ADC_WET  1000  // replace with your value
```

Skip this and your pump either never triggers or never stops. Both outcomes are bad for the plants and worse for your relationship with the project.

### 4. Flash and Verify

Serial Monitor at **115200 baud**. Healthy boot ends with:
```
[BOOT] Control active
T:26.1 H:61.4 S:42%(R:2241) P:OFF F:OFF | V:THS | WiFi:OK(10s) | Blynk:OK | TS:OK | Buf:1/15
```

**Troubleshooting:**

`[I2C] No devices found` — SDA/SCL wires swapped, or LCD not powered. TerraFirm auto-scans 0x27 and 0x3F — whichever responds first wins.

`V:--S` in status line — DHT11 has failed 5 consecutive reads. Check the 10kΩ pull-up on the DATA pin. A DHT11 without a pull-up is a small plastic rectangle that has given up.

`[WiFi] Disconnected — retry in Xs` — normal during outages. Local control continues. Cloud resumes automatically on reconnect.

`[PUMP] OFF — TIMEOUT` — pump hit the 30s safety limit. Either soil is very dry and needs a longer `PUMP_MAX_ON_MS`, or the soil sensor needs recalibration.

---

## 📁 Project Structure

```
terrafirm/
├── TerraFirm.ino   ← entire firmware, one file, flash this
└── README.md
```

---

## 🔮 What's Next

**DHT22 upgrade** — DHT11 is ±2°C, 1Hz max. DHT22 is ±0.5°C, wider range, same wiring. One `#define` change.

**Blynk threshold control** — move `PUMP_ON_MOISTURE`, `FAN_ON_TEMP`, etc. to Blynk data streams. Tune from phone without reflashing.

**OTA firmware updates** — ArduinoOTA so changes don't need physical USB access to a controller mounted inside a greenhouse.

**Multi-zone support** — two soil sensors, two pump channels, independent thresholds. The circular buffer architecture already supports it.

**Irrigation fault alert** — if `pumpDuty > 80%` for two consecutive uploads and `avgSoil` hasn't moved, push a Blynk notification. Something is physically wrong with the line.

**CO₂ monitoring** — MH-Z19B via UART. CO₂ inside a sealed greenhouse tanks on cloudy days when photosynthesis slows. Worth knowing before the plants do.

---

## 📋 Requirements

```
BlynkSimpleEsp32    >= 1.3.2
LiquidCrystal_I2C   >= 1.1.2
DHT sensor library  >= 1.4.4
ThingSpeak          >= 2.0.0
esp_task_wdt        (ESP32 Arduino Core >= 2.0.0, built-in)
```

---

## 📄 License

MIT — grow things, break things, document the breaking.

---

## 🙏 Acknowledgements

- **Espressif** — ESP32 hardware and Arduino core
- **Blynk** — IoT platform for real-time remote control
- **MathWorks / ThingSpeak** — time-series data logging
- **Adafruit** — DHT and LCD libraries
- **IN4007** — the unsung hero of this entire build
- Every plant that died during development so future plants don't have to
