# Smart AC Controller: ESP32 + DHT11 + Relay + MQTT + Node-RED

The ESP32 reads temperature and humidity. It switches the air conditioner on with a relay when the temperature goes **above 30 °C** and switches it off when the room has cooled down. Everything is monitored and controlled remotely from a **Node-RED dashboard** over **MQTT**.

---

## 1. Architecture

```
 ┌──────────────── WOKWI (ESP32) ────────────────┐                    ┌────────── NODE-RED ──────────┐
 │ DHT11/22 ─► ESP32 ─► Relay ─► AC (blue LED)   │   publish          │  mqtt in ─► function ─► UI   │
 │              │  ├─► LCD 16x2 (I2C)           │ ─────────────────► │  gauges, chart, status card, │
 │              │  ├─► Red LED (T > threshold)  │   telemetry/event  │  event log, notifications    │
 │              │  ├─► Green LED (MQTT OK)      │                    │                              │
 │  button ─────┘  └─► Buzzer                   │ ◄───────────────── │  switches/slider ─► mqtt out │
 └───────────────────────────────────────────────┘   cmd/ac, cmd/mode └──────────────────────────────┘
                              ▲                    cmd/setpoint                   ▲
                              └──────────── MQTT broker: broker.hivemq.com:1883 ──┘
```

## 2. Features (what makes this project stand out)

| Feature | Why it matters |
|---|---|
| **Hysteresis**: ON above 30 °C, OFF below 29 °C | Without it the relay would turn on and off over and over around 30.0 °C, which damages a real compressor |
| **AUTO / MANUAL modes** | AUTO follows the temperature rule. MANUAL lets the user force the AC ON/OFF |
| **Adjustable threshold** (16–40 °C) from the dashboard | Saved in the ESP32 flash (NVS), so it survives a reboot |
| **Closed-loop feedback** | Dashboard switches show the real state reported back by the ESP32, not only what was clicked |
| **Last Will & Testament (LWT)** | The broker announces "offline" automatically if the ESP32 dies |
| **Fail-safe** | If the sensor fails in AUTO mode, the AC is switched OFF and an alarm is raised |
| **Compressor protection** | Minimum delay between automatic switchings (5 s in the simulation, about 3 min on a real AC) |
| **Non-blocking code** | `millis()` instead of `delay()`. AUTO control keeps working even if the internet drops |
| **Local interface** | LCD, 3 LEDs, buzzer beep on every switching, and a physical ON/OFF button |
| **Heat index, AC run-time, switch counter, RSSI** | Extra useful information on the dashboard |
| **Event log + pop-up notifications** | Every AC ON/OFF event is shown with its time and reason |

## 3. Hardware / wiring

| Component | ESP32 pin |
|---|---|
| DHT sensor DATA (VCC → 3V3, GND → GND) | GPIO 15 |
| Relay module IN (VCC → 5V) | GPIO 26 |
| Relay COM → 3V3, NO → 220 Ω → **blue LED = the AC unit** | (through the relay contact) |
| Red LED (over-temperature alarm) + 220 Ω | GPIO 27 |
| Green LED (MQTT connected, blinks while reconnecting) + 220 Ω | GPIO 25 |
| Buzzer | GPIO 14 |
| Push button (manual AC ON/OFF) | GPIO 13 (internal pull-up) |
| LCD 1602 I2C (address 0x27) | SDA 21, SCL 22 |

> **Note on DHT11:** Wokwi only has the **DHT22** part. The DHT11 and DHT22 have the **same pinout and the same 1-wire protocol**, so the circuit is identical. For real hardware with a DHT11, change one line in the code: `#define USE_DHT11 1`.
>
> **Real AC safety:** The relay contact (COM/NO) is wired in series with the AC power line. 230 V wiring must be done by a qualified person. In the simulation, the blue LED stands in for the AC load.

## 4. MQTT topics

Topic root: `iot2026/smartac/student01` (**change `student01` to your own name** in both `sketch.ino` and `node-red-flow.json`. The public broker is shared with the whole world.)

| Topic | Direction | Payload | Notes |
|---|---|---|---|
| `…/telemetry` | ESP32 → | JSON, every 2 s | `{"temperature":31.2,"humidity":45,"heatIndex":32.4,"ac":true,"mode":"AUTO","setpoint":30,"hysteresis":1,"alarm":true,"sensorOk":true,"acRuntime":120,"switches":3,"rssi":-60,"uptime":300}` |
| `…/status` | ESP32 → | `online` / `offline` | retained, LWT |
| `…/ac/state` | ESP32 → | `ON` / `OFF` | retained |
| `…/event` | ESP32 → | JSON | `AC_ON`, `AC_OFF`, `MODE`, `SETPOINT`, `SENSOR_ERROR`, `BOOT` |
| `…/cmd/mode` | → ESP32 | `AUTO` / `MANUAL` | |
| `…/cmd/ac` | → ESP32 | `ON` / `OFF` | switches to MANUAL automatically |
| `…/cmd/setpoint` | → ESP32 | e.g. `30` | range 16–40 °C |

## 5. Control logic

```
                 T > threshold (30 °C)
      ┌────────┐ ───────────────────────► ┌────────┐
      │ AC OFF │                          │ AC ON  │
      └────────┘ ◄─────────────────────── └────────┘
                 T < threshold − 1 °C (29 °C)

 29 °C ≤ T ≤ 30 °C  → dead band: keep the current state
 Sensor error       → AC OFF (fail-safe)
```

## 6. How to run it

### A. Wokwi
1. Go to <https://wokwi.com> → **New project** → **ESP32**.
2. Replace `sketch.ino` and `diagram.json` with the files from this folder.
3. Click the **Library Manager** tab (or create `libraries.txt`) and add: `PubSubClient`, `DHT sensor library for ESPx`, `LiquidCrystal I2C`.
4. Press **▶ Start**. The Serial Monitor shows WiFi → MQTT → telemetry.

### B. Node-RED
1. Install: `npm install -g --unsafe-perm node-red`, then run `node-red`. Open <http://localhost:1880>.
2. Menu ☰ → **Manage palette** → **Install** → `node-red-dashboard`.
3. Menu ☰ → **Import** → paste `node-red-flow.json` → **Import** → **Deploy**.
4. Open the dashboard at **<http://localhost:1880/ui>**.

## 7. Demo scenario (for the presentation)

1. Start the simulation. The dashboard shows **ONLINE** and the gauges move. The AC is **STOPPED** (26 °C).
2. Click the DHT sensor in Wokwi and drag the temperature to **32 °C**. The red LED lights up, the relay clicks, the blue AC LED turns ON, the buzzer beeps, the dashboard shows a notification and the fan icon spins.
3. Drop the temperature to **29.5 °C**. The AC **stays ON** (hysteresis dead band). Drop it to **28 °C** and the AC turns OFF.
4. Move the **threshold slider** to 25 °C. The AC turns ON at 26 °C, which shows the threshold is configurable.
5. Toggle the **AC power** switch on the dashboard. The mode changes to **MANUAL** and the AC obeys the dashboard.
6. Press the **blue button** in Wokwi. The AC toggles locally and the dashboard updates instantly.
7. Click **Reset: AUTO @ 30 °C** to return to the original behaviour.
8. Stop the simulation. Within about 45 s at most (1.5 × the 30 s keep-alive), the broker publishes the LWT, the dashboard shows **OFFLINE** and a warning pops up.

## 8. Files

| File | Content |
|---|---|
| `sketch.ino` | ESP32 firmware (Arduino) |
| `diagram.json` | Wokwi circuit |
| `libraries.txt` | Wokwi library list |
| `node-red-flow.json` | Node-RED flow + dashboard (import it) |
