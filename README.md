# AirRing — Monitor SEN66

**Printable enclosure on MakerWorld: [AirRing](https://makerworld.com/en/models/3199590-airring-air-quality-monitor-mqtt)**

Air-quality monitor for the **Waveshare ESP32-S3-Touch-AMOLED-1.75**
(466×466 round) with a **Sensirion SEN66**, a self-contained display and
**Home Assistant** auto-discovery over MQTT.

> **This is a fork** of [socquique/Monitor-SEN66](https://github.com/socquique/Monitor-SEN66).
> It adds security hardening for the web panel, an optional battery-saver
> mode, a German default UI with English log output, Home Assistant entity
> names aligned with [Project Aura](https://github.com/21cncstudio/project_aura),
> and fixes for two bugs that only show up on real hardware (a touch-driver
> abort during WiFi start-up, and fan cleaning that never actually ran).
> See [What changed in this fork](#what-changed-in-this-fork) for the full list.

Inspired by Scoolt96's [PowerDot Air](https://makerworld.com/es/models/3029930-powerdot-air-home-assistant-air-sensor),
which does the same on the Waveshare 1.46" LCD. Its
[firmware is closed](https://github.com/Scoolt96/PowerDot-fw) (the repo only
publishes `.bin` files), so this is a new implementation from scratch,
adapted to the round AMOLED: CO5300 driver over QSPI, CST9217 touch, an RTC
for the history, and a desktop simulator to iterate on the screen.

<p align="center">
  <img src="docs/img/pagina_0.png" width="300" alt="Overview page">
</p>

## What it does

- Measures **nine quantities** with a single sensor: CO₂, PM1.0 / PM2.5 /
  PM4.0 / PM10, VOC index, NOx index, temperature and humidity — plus noise
  from the board's own microphones.
- **Six pages** on a round AMOLED screen, plus an idle view that shows
  everything at a glance.
- **Shows up in Home Assistant by itself** via MQTT auto-discovery, and also
  works with **HomeKit** through Homebridge ([guide](docs/HOMEBRIDGE.md)).
- **Everything local**: no cloud, no account, no telemetry. Keeps working with
  the network down — the display is autonomous.
- **Web panel** for configuration and **over-the-air updates** (OTA), with
  automatic rollback if a new image fails to boot.
- **Audible alert** when CO₂ crosses a threshold, with hysteresis.
- Runs **on battery**, with a power-saving profile that kicks in when you
  unplug (~6 h measured with a 1000 mAh cell), and an optional battery-saver
  mode that cycles the sensor to roughly double that.
- The display speaks **German, English and Spanish**.

| Overview | CO₂ | Particulates |
|---|---|---|
| ![](docs/img/pagina_0.png) | ![](docs/img/pagina_1.png) | ![](docs/img/pagina_2.png) |
| **Gases** | **Climate** | **Idle** |
| ![](docs/img/pagina_3.png) | ![](docs/img/pagina_4.png) | ![](docs/img/reposo.png) |

*(screenshots from the included simulator, which runs the same UI as the firmware)*

## Parts

| Part | Notes |
|---|---|
| Waveshare ESP32-S3-Touch-AMOLED-1.75 | SKU 31261 (the -B and -G variants work too) |
| Sensirion SEN66 | 3.3 V ±5 %, I2C, ships with a 6-wire JST GH cable |
| 4 wires to the 8-pin header | 3V3, GND, SDA, SCL |
| 3 × M2×6 screws | Hold the board to the bezel ring |
| [AirRing enclosure](https://makerworld.com/en/models/3199590-airring-air-quality-monitor-mqtt) | Printable, 0.24 mm layers, 2 walls, 15 % infill |
| USB-C cable | power and flashing |

## Wiring — read before connecting

**The SEN66 answers at I2C address `0x6B`, which is exactly the same address
as the QMI8658 IMU soldered onto the board.** They cannot share a bus. That is
why the sensor goes on a **second I2C bus** (port 1) on expansion-header GPIOs
at 100 kHz (the maximum the SEN66 supports), while the on-board bus (touch,
PMU, RTC, IMU, audio) stays at 400 kHz. Bonus: the sensor never competes with
the touch controller.

**The full diagram is in [docs/WIRING.md](docs/WIRING.md).** Summary:

Wire by the **silk-screen label** on the header, not by pin number:

| SEN66 | Wire | → | Label on the board |
|---|---|---|---|
| 1 VDD | red | → | **3V3** (not VBUS, that's 5 V!) |
| 2 GND | black | → | **GND** |
| 3 SDA | green | → | **IO17** |
| 4 SCL | yellow | → | **IO18** |

The blue and purple wires (sensor pins 5 and 6) are left unconnected. Careful:
the actual header order is `IO18 IO17 IO16 RXD TXD 3V3 GND VBUS` and does
**not** match the 1..8 numbering in the
[official hardware reference](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/HARDWARE_REFERENCE.md).

If you wire it differently there is no need to guess: at boot, if the sensor
does not answer on those pins, the firmware **scans the candidate pairs** on
the header and tells you on the console which one works:

```
W (1234) sen66: not responding at SDA=17 SCL=18, scanning header
I (1500) sen66: product: 'SEN66'
W (1500) sen66: found at SDA=16 SCL=17 (not the board.h pins); set BOARD_SEN66_PIN_* to these values
```

Put the working pair into `board.h`, rebuild, done. The check does not stop at
the address ACK: it reads the product name and requires it to start with
`SEN6`, so it cannot be fooled by some other chip.

The SEN66 has a fan and **Sensirion warns of peaks up to 350 mA**. Waveshare
does not specify how much current the header's 3V3 can supply; in practice
the AXP2101's buck has plenty of headroom, but power the board from a USB-C
charger rated 1 A or more, not from a hub port. If you see brownout resets,
CRC errors or `fan_error`, that is the cause, and the fix is a separate 3.3 V
supply for the sensor with a common GND. On battery, expect hours, not days.

## Build and flash

Requires **ESP-IDF 5.3 or newer** (tested with 5.5.2).

```bash
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor      # Linux; on macOS the port is /dev/cu.usbmodemXXXX
```

The component manager fetches LVGL 9, `esp_lvgl_port`, `esp_lcd_co5300` and
`esp_lcd_touch_cst9217` by itself.

The partition table has **two 4 MB app slots**, so OTA updates from the
browser work from the very first flash. With `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`
set (it is, in `sdkconfig.defaults`), a new image boots on probation: if it
does not reach the end of `app_main` and confirm itself, the bootloader falls
back to the previous image on the next reset. That saved this fork once
already during development.

## First boot

1. With no network configured, the device opens a **WPA2** access point called
   **`SEN66-XXXXXX`**. The screen shows the network name **and an 8-digit
   password**, which is generated fresh on every boot.
2. Connect to it and open **http://192.168.4.1**.
3. Fill in WiFi and, optionally, the MQTT broker (`mqtt://192.168.1.10:1883`),
   then save. The device reboots and connects.
4. From then on the web panel is at the IP shown on the screen.

If the WiFi password changes and the connection fails 8 times in a row, the
setup portal opens again by itself without losing the configuration — no
reflashing needed. It closes automatically as soon as the network is back.

## Home Assistant

You need an MQTT broker: how to set one up and how to verify that the device
is publishing is in **[docs/MQTT.md](docs/MQTT.md)**. With that in place:

1. In HA, Settings > Devices & services > **Add integration > MQTT**, pointing
   at the broker with its username and password.
2. That's it. **No YAML to touch.**

On connect, the firmware publishes retained discovery messages to
`homeassistant/sensor/sen66-xxxxxx/<key>/config` and HA creates **one device
with 11 entities**: the nine from the sensor, the overall air-quality level
and the WiFi signal as a diagnostic.

Entity names follow the same scheme as Project Aura, so dashboards written
for one work with the other: `Temperature`, `Humidity`, `CO2`, `VOC Index`,
`NOx Index`, `PM1.0`, `PM2.5`, `PM4.0`, `PM10`, `Noise`, and `Air Status` for
the overall level.

**Noise** appears as one more entity, with device class `sound_pressure` in
dB. Not in HomeKit: there is no such thing as a noise level there, so it goes
as a threshold binary instead (see [docs/HOMEBRIDGE.md](docs/HOMEBRIDGE.md)).

**With a battery connected, 4 more entities appear**, also as diagnostics:
charge in %, cell voltage in mV, and two binaries for *charging* and *USB
power*. If there is no cell they are **not published**, so Home Assistant is
not left with four entities that will never say anything. This is detected
when connecting to the broker, so if you add the battery later, reboot the
device.

- State: `sen66-xxxxxx/state`, one JSON every 10 s.
- Availability: `sen66-xxxxxx/status` with a *last will*, so if the device
  goes down HA marks it unavailable instead of leaving frozen values.

Device classes are set (`carbon_dioxide`, `pm25`, `pm10`, `temperature`,
`humidity`…) so graphs and units come out right. The VOC/NOx indices have no
class because HA has none for them; they get an icon instead.

The level entity shows the raw identifier — `good`, `fair`, `moderate`,
`poor`, `bad` — because that is what travels over MQTT, untranslated, so that
automations do not break when you change the display language. If you want
it in plain language **on the dashboard, without losing that**, use a
template in `configuration.yaml`:

```yaml
template:
  - sensor:
      - name: "Air quality (text)"
        state: >
          {{ {'good':'Good','fair':'Fair','moderate':'Moderate',
              'poor':'Poor','bad':'Very poor'}.get(
              states('sensor.monitor_sen66_air_status'), 'Unknown') }}
```

For automations, use the identifier, not the text:

```yaml
trigger:
  - platform: numeric_state
    entity_id: sensor.monitor_sen66_co2
    above: 1200
```

> If you are upgrading an existing installation from the upstream firmware:
> the `unique_id`s are unchanged, so HA keeps history and automations. Only
> the friendly names update. Entities you had renamed by hand keep your name.

### History

There is an easy misunderstanding here. HA stores **two different things**:

- **Long-term statistics**: one row per hour with min, max and mean. **Never
  purged.** They appear automatically for every metric because the firmware
  publishes `state_class: measurement`. You already have months of trends
  without touching anything.
- The **detail**, every sample as it arrived. That does expire, after
  **10 days** by default.

So raising the retention only lets you zoom into a specific day from a while
ago; it does nothing for the long trend.

If you still want it longer, in `configuration.yaml`:

```yaml
recorder:
  purge_keep_days: 30
  commit_interval: 30
  exclude:
    entities:
      - sensor.monitor_sen66_wifi
```

The device publishes every 10 s and its metrics almost always change, so on
its own it produces about **15,000 rows a day** (measured). With 30 days the
database ends up around 80 MB. The WiFi signal is excluded because it is
~1,700 daily rows of a diagnostic nobody looks at historically; it is still
visible live, just not stored.

## HomeKit (without Home Assistant)

If instead of HA you want Apple's **Home** app and Siri, it works through
Homebridge and `homebridge-mqttthing`: same broker
([docs/MQTT.md](docs/MQTT.md)), different listener. Ready-to-paste accessories
and the pitfalls are in **[docs/HOMEBRIDGE.md](docs/HOMEBRIDGE.md)**.

In that case leave the **discovery prefix empty** in the device's panel:
auto-discovery is only understood by HA.

## Languages

The display speaks **German, English and Spanish**, selectable in the web
panel. **German is the factory default** in this fork; devices with a saved
configuration keep whatever language they had.

The web panel itself is in German. Serial log output is in English. Source
comments are in Spanish, as in the upstream project, so that a pull request
back to the original author stays readable to him.

LVGL's Montserrat fonts only ship ASCII, so German needs `ÄÖÜäöüß`. Rather
than regenerating all five fonts, there is a **fallback font** with just
those seven glyphs (`main/fonts/`, ~37 KB total) chained through
`lv_font_t.fallback`. LVGL's Montserrat fonts are `const` and live in flash,
so the UI uses **RAM copies** of ~30 bytes with the `fallback` field filled in
(`main/fonts/fonts.c`).

Spanish is written **without accents** on purpose: "Particulas", "ug/m3".

The value that goes over MQTT is **not translated**: it is a stable English
identifier (`good`, `fair`, `moderate`, `poor`, `bad`). If it changed with the
display language it would break Home Assistant automations.

## The display

Six pages, **swiped by finger, no auto-rotation**. After `screen_timeout_s`
without touching anything the screen dims and shows an **idle view with all
nine quantities** at once; touching it brings you back to exactly the page
you were on.

| Page | Contents |
|---|---|
| Overview | Traffic-light ring with the worst pollutant and which metric is driving it |
| CO₂ | Big value, 400–2400 ppm ring and last-hour graph |
| Particulates | PM2.5 large with ring and its curve behind; PM1.0 / PM4.0 / PM10 below |
| Gases | VOC and NOx indices in two rings |
| Climate | Temperature and humidity, both curves overlaid with their own scales |
| Noise | Level in dB with a 30–90 ring and graph; see the calibration note |

On AMOLED, black is a switched-off pixel, so the black background costs
nothing and the idle dimming really saves power.

### Thresholds

Five levels: **GOOD / FAIR / MODERATE / POOR / VERY POOR**.

| Metric | Boundaries |
|---|---|
| CO₂ (ppm) | 800 · 1000 · 1400 · 2000 |
| PM1.0 and PM2.5 (µg/m³) | 10 · 20 · 25 · 50 |
| PM4.0 and PM10 (µg/m³) | 20 · 40 · 50 · 100 |
| VOC index | 150 · 250 · 400 · 450 |
| NOx index | 20 · 150 · 300 · 400 |

Temperature and humidity are not pollution: they are rated by distance from
the comfort range (19–25 °C, 40–60 %RH) and **do not enter the overall
traffic light**.

The thresholds live in one place, the `k_bands` table in
[`main/air.c`](main/air.c).

## Desktop simulator

Builds **the same UI and the same logic** as the firmware against SDL2, with
synthetic data (CO₂ rises, someone airs the room every 15 min, occasionally
there is cooking). Iterating on the screen here is much faster than flashing.

```bash
brew install sdl2                      # once (Linux: apt install libsdl2-dev)
idf.py build                           # once, so that LVGL gets downloaded
cd sim && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/sen66_sim
```

Useful options:

```bash
./build/sen66_sim --lang de            # in German
./build/sen66_sim --page 2             # open on a specific page
./build/sen66_sim --scenario bad       # bad air
./build/sen66_sim --warp 60            # time runs 60x
./build/sen66_sim --offset 2400        # advance the cycle phase
./build/sen66_sim --shots /tmp/caps 5  # 5 BMP captures and exit
```

## Settings (web panel)

Network and MQTT, name in HA, language, time zone and NTP, normal and dimmed
brightness, time until dimming, seconds per page, visible pages (bit mask),
graph window, temperature offset, altitude, CO₂ auto-calibration, the
**audible alert** (enable, trigger threshold, clear threshold, volume), an
optional **panel password**, and the **battery-saver** cycle.

The two alert thresholds are deliberately not the same number. It triggers
on rising above `alarm_co2_ppm` and does not go quiet until falling below
`alarm_clear_ppm`; that gap is what stops it beeping endlessly while CO₂
hovers around the limit. If you save the clear threshold above the trigger
threshold, the panel lowers it by itself and says so in the log.

On save, **the device reboots**: it is the clean way to apply the network,
MQTT, pages and the sensor's own settings in one go, with nothing left
half-applied. Brightness does apply immediately.

There are also buttons to **clean the sensor's fan**, **test the speaker**
and **upload a `.bin`** for an OTA update. The cleaning also runs **by itself
once a week**, as Sensirion recommends; the date of the last one is kept in
NVS so a reboot does not restart the count.

Fan cleaning stops the measurement first (the SEN66 only accepts the command
in idle mode), runs the fan at full speed for 10 seconds, and restarts —
about 12 seconds without readings, and you can hear it. In the upstream
firmware the command was sent while measuring, where the sensor silently
ignores it, so neither the button nor the weekly cleaning ever did anything.

### Battery saver

Optional and off by default. When enabled, it only acts **without USB**: the
sensor measures for a window (default 180 s) and then sits idle for the rest
of a cycle (default 600 s). In measurement mode the SEN66 draws ~90 mA (fan,
laser, CO₂ cell); idle it draws ~3 mA, so 3 min in 10 brings the sensor's
average down to ~29 mA and roughly doubles battery life. The moment USB is
back, continuous measurement resumes.

The price, per the datasheet: NOx needs ~5 minutes of continuous operation
to react, so it is effectively blind in short windows; the CO₂ accuracy
guarantee with auto-calibration assumes continuous operation; and the first
~30 s of each window the PM readings are still settling. PM, temperature,
humidity and VOC (whose algorithm state survives stop/start) stay usable.
During the pause, Home Assistant keeps the last retained values and the
display shows "power saving: sensor paused".

### Recalibrating CO₂

Under Maintenance there is a button to force a CO₂ calibration against a
known reference. **It only makes sense with a real reference**: outdoor air
is about 420 ppm, so take the device outside, away from people, let it
measure for five minutes and recalibrate to 420. With a made-up number you
break the measurement instead of fixing it, **and it is stored in the
sensor's EEPROM**.

It is rarely needed: automatic self-calibration is on and adjusts itself as
long as the device sees fresh air now and then.

Internally, the panel only *requests* the recalibration; the sensor task
executes it, because the command requires the measurement to be stopped and
doing the stop/start from the server thread would collide with the
once-a-second read. The VOC learning state is saved before stopping and
restored afterwards, so a recalibration does not throw away days of
learning. The result (the correction applied, in ppm) shows up in the panel.

### Panel security

The upstream firmware has no password on purpose and says so; this fork
tightens that without changing the default experience:

- **Optional Basic Auth.** Set a user and password under "Panel access". With
  a password set, every route requires it — except during the setup portal,
  where physical presence is assumed. With no password, the panel stays open
  on the local network as before.
- **Cross-site protection.** Every API call from the panel carries a custom
  `X-CSRF` header, which a page on another site cannot add without a CORS
  preflight the device does not answer. The `Host` header must be the
  device's own IP (or `192.168.4.1`), which blocks DNS-rebinding attacks.
- **The backup with passwords is refused unless a panel password is set**,
  so your WiFi key cannot be downloaded in clear text from an unprotected
  panel.
- **The rescue portal is WPA2**, not open, so a neighbour in range cannot
  reconfigure the device while it is trying to reconnect. It closes itself
  once the network is back.
- **OTA rollback** is enabled: an image that boots but fails before
  confirming itself is reverted automatically.
- **Signed OTA** is prepared but commented out in `sdkconfig.defaults`,
  because it needs a signing key you generate yourself
  (`espsecure.py generate_signing_key`). Until you enable it, `/api/ota` is
  protected by the checks above but not cryptographically.

Free-text fields (device name, SSID, time zone…) are JSON-escaped in the
panel and in the HA discovery payload, and the response builders cannot
overrun their buffers.

## Sensor set-up

- **The VOC and NOx indices need time.** They are adaptive indices: the
  sensor learns the usual environment and places it at 100 (VOC) and 1
  (NOx). For the first few hours the values do not mean much.
- **Temperature will read high**, because the sensor sits inside an
  enclosure next to electronics that give off heat. Compare with a reference
  thermometer and put the difference into "Temperature offset": the offset
  is programmed **inside the SEN66**, so it corrects relative humidity too.

  But before applying anything, read [On measuring the
  deviation](#on-measuring-the-deviation): it is easy to over-correct.
- **Altitude**: affects the CO₂ measurement. Enter the site's metres.
- **CO₂ auto-calibration (ASC)**: on by default. It assumes the device sees
  fresh air (~400 ppm) regularly. In a room that is never aired, turn it off
  and do a forced recalibration outdoors.

### The sound-level meter

The board carries **two microphones**, which do not hang off the ES8311 (that
is output only) but off a separate **ES7210** at `0x40`. Noise comes in as
one more metric, so it inherits history, graphs, web panel and Home Assistant
discovery with nothing special; the only bespoke parts are the driver and the
conversion to decibels. **It does not count toward the overall traffic
light**: it is not air quality.

**The level is calibrated** (2026-09-09). The microphone yields a
full-scale level (dBFS, always negative); converting that to dB SPL requires
the microphone's sensitivity and the codec's gain, and that is measured, not
derived. The default offset is **112 dB**, from two anchors using a phone
sound-meter app held against the device, energy-averaging a window of each:

| | reference | monitor | offset |
|---|---|---|---|
| ambient | 54.6 dB | −57.8 dBFS | 112.4 |
| music | 64.2 dB | −48.1 dBFS | 112.2 |

Two tenths apart over a 10 dB range: **it is a constant shift, not a
slope**, which is exactly what a single number can correct. Verifying that was
the point of using two levels rather than one — with CO₂ the deviation turned
out to be a slope, and there no offset helps.

Two honest caveats. The reference is a phone, not a class-2 meter: the zero
may be off by a couple of dB, the same for everything. And you must average
windows of tens of seconds on both sides simultaneously; with music, two
instantaneous readings a few seconds apart gave offsets of 95 and 111. The
"Noise calibration" field in the panel lets you readjust against a better
standard.

**What does NOT work for calibrating this is another consumer meter in the
house.** It was tried against a Qingping and there is no way: across 1,983
paired samples the correlation is 0.583, and in the final test the monitor
reached −41.6 dBFS (26 dB above its floor) while the Qingping stayed pinned
at its 36 dB, alive and well. CO₂ and temperature equalise in a room; **noise
does not** — it depends on where each device sits. An earlier offset of 102
was estimated from that useless comparison, and it was wrong.

**The number is instantaneous, not an average.** What is published is the
RMS of the last 125 ms as-is, a sound meter's "fast" weighting. It jumps a
lot: within a single window of a quiet room it ranges from −69 to −55 dBFS.
It is not comparable with a consumer meter that averages over minutes —
which is why this one can show 53 dB while another on the same table shows
37 without either being wrong. A short peak lifts one and not the other.

**The device's floor is at about 43 dB** (−69.2 dBFS measured with the fan
stopped). Below that it cannot distinguish: it reports its own noise.

**And there is no A-weighting.** The calculation is the raw RMS of the
signal; a sound meter, a phone app or the Qingping apply the A curve, which
discards low frequencies because the ear does not hear them either. With
broadband music the difference is small — which is why at 75 dB it matched
the Qingping within 2 dB. In a quiet room what remains is precisely low
frequencies (fridge, distant traffic, the power supply) and there the A
curve easily subtracts 10–15 dB.

So the excess in silence has **two candidate causes**, the microphone floor
and the lack of weighting, and with this data they cannot be separated: a
phone app showed a 28 dB minimum where the monitor does not go below 43.
Whichever dominates, the practical conclusion is the same: **below ~50 dB
the number is not meaningful**, from 55 upwards it is. Adding A-weighting to
the filter in `mic.c` is the next step if the low range ever matters.

**About the SEN66's fan**, which sits inside the same enclosure: the
reasonable suspicion was that it sets the noise floor. It was measured by
stopping the measurement (`/api/fan`) and comparing. **First attempt
(Aug 2026), failed**: with short windows it came out that spinning measured
LESS than stopped, which is impossible; room variation was in charge.

**Second attempt (2026-09-09), with a number**: eight 45 s windows
alternating ON and OFF, looking at the **minimum**, which is the statistic
that matters for a floor (the energy mean is driven by the room's peaks, not
the device).

| | minima, dBFS | mean | range |
|---|---|---|---|
| fan ON | −68.0 −68.0 −68.4 −69.1 −69.3 | −68.6 | 1.3 |
| fan OFF | −69.2 −69.6 −69.6 | **−69.5** | **0.4** |

**All three OFF fall below all five ON**, and they are also much more
repeatable among themselves — exactly what you expect if stopping the fan
leaves only the microphone's own noise. Solving for it, the fan alone is at
about **36 dB SPL**: fifteen below what a normal room shows.

**Conclusion: real but irrelevant**, not "inaudible for lack of method" as
it was closed the first time. Honestly: the difference is 0.9 dB and the ON
readings themselves vary by 1.3 dB between windows, so it still brushes the
limit of what the method resolves. What no longer fails is the sign.

### The enclosure and the air

The SEN66 has **two inlets and one outlet**, all three on the same face: the
square opening and the round membrane are the inlets, and the fan is the
outlet. Sensirion asks for two things that "don't cover them" alone does not
satisfy ([mechanical guide](https://sensirion.com/media/documents/EA641247/6977159F/PS_AN_SEN6x_Mechanical_Design_and_Assembly_Guidelines_D1.pdf)):

- **Separate the outlet from the inlets**, or the sensor ends up measuring
  the air it just expelled.
- **Isolate all three from the inside of the enclosure**, or the fan draws
  air from inside the device, warmed by the ESP32 and the display, instead of
  room air.

If you add ducts, the minimum areas are **56 mm² per inlet and 148 mm² at
the outlet**. And the only discouraged orientation is with the holes
**facing up**: dust falls in and the sensor ages sooner.

**The AirRing enclosure meets both, and it is verified by measurement**,
which is the only way to know:

- *It does not draw from inside*: temperature stays within **+0.4 °C** of an
  independent thermometer. If the air came from inside it would be several
  degrees.
- *It does not recirculate*: light an incense stick next to it and time the
  PM2.5 decay against a reference sensor. Decay constant **9.7 min versus
  8.7** for the other device. If it re-inhaled its own outlet, its tail would
  be much longer than the room's.

### On measuring the deviation

Here are 22 hours of real measurements against a Qingping Air Monitor 2 and
an independent thermometer, all three on the same table. They serve as a
warning, because every step of the way invited a correction that should not
have been made.

**Two devices give you the difference, never who is right.** Twelve hours
against the Qingping gave +1.05 °C and −8.4 % humidity, very constant, and
the obvious conclusion was to enter −1.1 °C. A third thermometer between the
two dismantled that: it read 26.1 °C and 56 %, i.e. the SEN66 at **+0.4 °C**
and the Qingping at −0.4 °C. The humidity mismatch was **the Qingping's**
(+8.9 %), not the SEN66's. Aligning one to the other would only have
propagated the error of whichever one was taken as the standard.

**A whole day, not a few hours.** That temperature difference was so stable
because there was only daytime data. With the full cycle it goes from
**+0.20 to +1.30 °C**: in the small hours it narrows because the other device
warms up and the SEN66 does not. It varies as much as the correction would
be worth, so a fixed offset is right at one hour and wrong at another.

**Check at several concentrations.** In the afternoon the SEN66 read 44 ppm
less CO₂ than the Qingping and it looked like a clear error. With the whole
night you can see it is not an offset but a **slope**:

| CO₂ (Qingping) | SEN66 | difference |
|---|---|---|
| 451 | 407 | −44 |
| 545 | 517 | −28 |
| 645 | 627 | −18 |
| 731 | 752 | +22 |
| 811 | 836 | +24 |

`SEN66 = 1.20 × Qingping − 138`, and **they cross at 675 ppm**. Neither is
"wrong": they have different gains and agree at the crossing point. With
data from a single range, either one looks like the wrong one.

**Do not confuse a well-calibrated sensor with a pinned one.** The SEN66
spent five afternoon hours stuck at 403 ppm, suspiciously the outdoor
baseline, and it looked as if auto-calibration had set its zero too low. The
night curve cleared it up: 403 → 500 → 642 → 720 → **831 at dawn**, the
textbook rise of a closed room with people sleeping. The afternoon really was
well ventilated and the sensor followed it.

**Conclusion: no offset is applied on this device.** The +0.4 °C against the
reference fits within the SEN66's own tolerance (±0.5 °C), and the
difference is not even constant. Correcting that would be fitting to noise,
with the added problem that an offset gets forgotten and stays there
forever.

## Structure

```
main/
  app_main.c      boot order, tasks, clock, battery-saver cycle, fan cleaning
  board.h         verified board pinout (do not re-derive)
  display.c       CO5300 over QSPI + CST9217 + LVGL port (fault-tolerant touch)
  sen66.c         sensor I2C driver (own bus, CRC-8, auto-detection)
  air.c           pure logic: metrics, thresholds, levels, colours
  history.c       24 h circular history in PSRAM
  ui.c            the six pages (LVGL only, no ESP-IDF)
  i18n.c          display strings in ES / EN / DE
  net.c           WiFi station + WPA2 setup portal + SNTP
  ha_mqtt.c       auto-discovery and publishing
  webcfg.c        web server, JSON API, auth/CSRF guards and OTA
  settings.c      NVS persistence
  rtc_pcf85063.c  RTC (real time without network)
sim/              SDL2 simulator reusing air.c, history.c and ui.c
```

`air.c`, `history.c` and `ui.c` include nothing from ESP-IDF: that is what
lets them compile unchanged on the PC.

## What changed in this fork

Relative to upstream, as of v1.8.4 (September 2026):

- **Security**: optional Basic Auth, `X-CSRF` + `Host` checks on the API,
  secrets export gated behind a panel password, WPA2 rescue portal that
  closes on reconnect, OTA rollback, signed-OTA config prepared, JSON
  escaping and overflow-safe response builders.
- **Battery saver**: optional cyclic idling of the SEN66 on battery.
- **Fan cleaning actually works**: the measurement is stopped first, from the
  sensor task, for both the button and the weekly run.
- **Touch no longer aborts the firmware**: `esp_lvgl_port` wraps the touch
  read in `ESP_ERROR_CHECK`, and the CST9217 NACKs once during WiFi PHY
  start-up on this board. The port's touch input is replaced by an equivalent
  one that treats a failed read as "no touch". Without this, every first boot
  of an OTA image was rolled back.
- **Languages**: German factory default, German web panel with proper
  umlauts, English log output.
- **Home Assistant**: entity names aligned with Project Aura (`Air Status`,
  `VOC Index`, …); `unique_id`s unchanged.
- **Backup** now also includes the noise calibration offset.

## Status

Compiles clean (ESP-IDF 5.5.2, ~1.6 MB, no warnings) and **runs on real
hardware**: the fork has been developed and verified on a Waveshare
ESP32-S3-Touch-AMOLED-1.75 with a SEN66 (sensor firmware 4.0), updated
between versions over OTA, with the rollback mechanism exercised for real.
Both bugs fixed here are easy to miss on a working device: the touch NACK is
timing-dependent and only bites on the first boot after an OTA, and an
ignored fan-cleaning command produces no error at all.

## Licence

**[PolyForm Noncommercial 1.0.0](LICENSE).** You may use, modify and share it
freely **for any non-commercial purpose**: personal use, research, teaching,
non-profit organisations. What you may not do is sell it or use it inside a
paid product or service.

To be clear: **this is not free software** in the OSI sense, precisely
because it restricts commercial use. That is a deliberate decision by the
original author, and this fork keeps it. If you want to use it commercially,
get in touch with the upstream author.

And no warranty of any kind: it is a home project, **not a certified
measuring instrument**. Do not use it for anything where someone's health
depends on the number it shows.

### What that licence does not cover

- The **fonts** in `main/fonts/` derive from Montserrat and remain under
  [SIL OFL 1.1](main/fonts/NOTICE.md).
- The **dependencies** (ESP-IDF, LVGL, the Espressif and Waveshare
  components) keep their own licences, which are permissive.
- If there is ever a **3D model** to print, it goes with its own licence:
  Creative Commons expressly discourages its licences for software, and
  conversely PolyForm is not meant for physical objects.

## Acknowledgements

- **[socquique](https://github.com/socquique/Monitor-SEN66)**, the original
  author, for the firmware this fork builds on — and for documenting his
  measurements and decisions so thoroughly that most of this README is his.
- **[PowerDot Air](https://makerworld.com/es/models/3029930-powerdot-air-home-assistant-air-sensor)**
  by Scoolt96, on the Waveshare 1.46" LCD, where the idea came from. Its
  firmware is closed, so there is not a line of it here: this is an
  independent implementation for a different display.
- **[Project Aura](https://github.com/21cncstudio/project_aura)** by
  21CNCStudio, whose Home Assistant entity naming this fork adopts.
- **Sensirion**, for publishing their drivers with real documentation. This
  firmware's SEN66 protocol is verified against
  [raspberry-pi-i2c-sen66](https://github.com/Sensirion/raspberry-pi-i2c-sen66).
- **Espressif**, for ESP-IDF and for the
  [es8311](https://components.espressif.com/components/espressif/es8311)
  component, from which the audio codec's register sequence was copied (it
  uses the old I2C API, so here it is rewritten on the new one).
- **Waveshare**, for publishing the board's
  [hardware reference](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75).
  Careful: the header pin order it gives does NOT match the silk screen; the
  copper wins.
