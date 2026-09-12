# Monitor SEN66 — working notes for this repo

Air-quality monitor: Waveshare ESP32-S3-Touch-AMOLED-1.75 (466×466 round,
CO5300 + CST9217) + Sensirion SEN66, with MQTT auto-discovery for Home
Assistant. ESP-IDF 5.5 + LVGL 9. See [README.md](README.md) for usage.

This is the **sellwanger fork** of
[socquique/Monitor-SEN66](https://github.com/socquique/Monitor-SEN66). Most of
what follows is the upstream author's own notes, translated; the sections
marked *(fork)* are additions. Keep both: his lessons still apply.

The upstream author refers to a private skill `esp32-desk-gadget` with the
board's hardware cheat-sheet and LVGL performance lessons. **It is not in this
repo.** The relevant facts have been folded into `board.h`, `display.c` and
this file.

## Language policy *(fork)*

Four languages coexist on purpose. Do not "fix" one into another.

| Where | Language | Why |
|---|---|---|
| Source comments | Spanish | Keeps pull requests to the upstream author readable to him |
| Serial log (`ESP_LOG*`) | English | What you paste into issues |
| Web panel, and every server string the panel can display | German | The user's language; real umlauts, the panel is UTF-8 |
| Display | ES / EN / DE via `i18n.c`, German factory default | Display fonts are ASCII + a 7-glyph German fallback |
| MQTT / HA entity names | English, Project Aura scheme | Dashboards written for Aura work here |
| Documentation (`README.md`, `docs/`) | English | |

A string that reaches the panel through `httpd_resp_send_err` on a
*navigation* (not a `fetch`) — the backup download, a 401 body — gets no
charset header, so keep those umlaut-free. Strings returned to `fetch()` may
use umlauts freely.

## Invariants — do not break

- **`main/board.h` is the source of truth for the pinout**, verified across
  four projects and against the
  [official hardware reference](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/HARDWARE_REFERENCE.md).
  Do not re-derive pins or "correct" them by eye.
- **Only GPIO16, 17 and 18 are free on the header** (plus UART0 on 43/44).
  GPIO13 is `LCD_TE` and GPIO21 is `QMI_INT2`: they look free and aren't.
- **The SEN66 goes on I2C bus 1, never bus 0.** It shares address `0x6B` with
  the board's QMI8658 IMU. And at 100 kHz: that is its maximum.
- **`air.c`, `history.c` and `ui.c` must not include anything from ESP-IDF.**
  That is what lets the simulator build the same UI and logic. If something
  in the UI needs the platform (brightness, time), it comes in as a callback
  or a parameter, not as `#include "esp_*.h"`.
- **The display is initialised BEFORE WiFi is brought up** (`app_main`). DMA
  buffers need contiguous internal RAM and so does TLS; if WiFi goes first, it
  takes it.
- **ASCII only in on-screen UI text.** LVGL's Montserrat fonts have no
  accents, no `¿`, no micro sign. That is why the screen reads "Particulas"
  and "ug/m3" while MQTT carries "µg/m³" and "°C" (`air_metric_unit_ui` vs
  `air_metric_unit_ha`). German umlauts come from the fallback font in
  `main/fonts/`.
- **Never write to NVS from the UI loop**: it freezes both cores for ~1 s.
  `settings_save()` is only called from the web server and from `sensor_task`
  (fan-cleaning timestamp).
- **Anything that needs the sensor stopped runs in `sensor_task`** *(fork)*:
  CO₂ recalibration, fan cleaning, the battery-saver cycle. The web handlers
  only queue a request. Stop/start from the HTTP thread would collide with
  the once-a-second read.
- **New `settings_t` fields go at the END of the struct, and `CFG_VERSION`
  stays at 1** *(fork)*. `settings_load()` accepts shorter blobs and keeps
  defaults for the tail, so an update never wipes WiFi/MQTT. Bumping the
  version would.
- **Do not use `lvgl_port_add_touch()`** *(fork)*. `display.c` registers its
  own indev. The port wraps the touch read in `ESP_ERROR_CHECK`, and on this
  board the CST9217 NACKs once during WiFi PHY start-up; that single NACK
  used to `abort()` the firmware. Same INT/event wiring, tolerant read.
- **Every new API route goes through `guard_api()`** *(fork)*: Host check,
  `X-CSRF` header, optional Basic Auth. Routes reached by navigation
  (`/api/backup`) cannot carry the header and use `host_ok()` + `auth_ok()`
  only. Every `fetch()` in the panel JS sends `headers:H`.
- **A new OTA image must reach the end of `app_main`** *(fork)*.
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is on; the image confirms itself
  right after `sensor_task` is created. A panic before that point rolls back
  to the previous image — silently, from the panel's point of view. Read the
  serial log through the reboot when an OTA "does nothing".
- **In `app_main.c`, declare before use.** Two builds failed on exactly this:
  a `static` introduced next to its logic but used by a function higher up in
  the file. File-scope state goes at the top with the other statics.

## Round-panel things already paid for

- Flush windows must be **2 px aligned** or the AMOLED paints green lines at
  the edges (`round_area_cb` in `display.c`).
- At boot, **sweep the 480×480 GRAM to black**: the panel's memory is larger
  than the 466 visible pixels and `set_gap` does not cover every case.
- **Usable radius**: `RING_D` is 380 and the clock and network state go
  INSIDE the ring (`LV_ALIGN_CENTER, -146` and `-124`). The ring used to be
  322 precisely because at a larger diameter the arc crossed the status line
  and the text became unreadable over the colour; moving the chrome inside
  is what lets the arc reach almost to the edge.
- **The free space inside the ring is `RING_INNER_R` = 168**, not 190:
  `RING_D/2` is the outer edge and you subtract the 22 px arc thickness.
  Every page coordinate derives from that.
- **Charts bleed to the edge and sit under everything**: 430 px wide (wider
  than the ring itself), created BEFORE the rest so they stay at the back.
  The line passes behind the arc's arms and the glass clips it, which is the
  PowerDot effect. Careful: crossing the centre it hides the number — it has
  to be a lower band (y≈+100), tested.
- **That lower band eats any text that falls inside it.** At 90 px high
  centred on +100, the chart occupies +55 to +145. Two pages have text there:
  gases (the hint "100 = typical air", at +118) and particulates (the
  PM1.0/PM4.0/PM10 figures, +57 to +97). On both, the curve struck them
  through. Fix: an opaque background in the screen colour behind the text,
  which is what the ring already does on CO₂; the line passes behind and
  shows through the gaps. Before putting a chart on a page, check what lives
  between +55 and +145.
- **The two rings are not the same thing.** The overview ring is a traffic
  light drawn in full with an indicator the same thickness as the track; the
  measurement rings have the indicator 8 px thinner, because at equal
  thickness a small value draws a loose blob that looks like a render fault.
- `lv_chart` works with integers. For metrics with decimals (temperature, PM)
  **scale ×10** before feeding them or the curve comes out stepped
  (`chart_refresh`).

## Workflow

Iterate on the UI in the simulator, not by flashing:

```bash
cd sim && cmake --build build -j && ./build/sen66_sim --scenario bad
./build/sen66_sim --shots /tmp/caps 5   # screenshots to review without eyes
```

The simulator needs `idf.py build` to have run once, so the component manager
leaves LVGL in `managed_components/`.

Firmware (Linux build VM; on macOS the port is `/dev/cu.usbmodemXXXX`):

```bash
. ~/esp/esp-idf/export.sh       # per shell; alias it as `getidf`
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

### Updating a running device *(fork)*

OTA from the panel (Maintenance → upload `.bin`), or from the build VM:

```bash
curl -u admin -H 'X-CSRF: 1' -H 'Expect:' \
     --data-binary @build/monitor_sen66.bin http://<device-ip>/api/ota
```

`-u` is needed once a panel password is set; without it the device answers
401 before reading the body and curl reports a reset. `-H 'Expect:'` keeps
curl's timing like the browser's. Watch `idf.py monitor` through the reboot:
you want `Loaded app from partition at offset 0x…`, `v<new>`, and
`OTA image confirmed (rollback cancelled)`. If the old version comes back,
the new one panicked before confirming — the log has the backtrace.

### Versions and patches *(fork)*

- Every change that lands on a flashed device bumps `APP_VERSION` in
  `main/version.h` (1.8.x for fixes, 1.9.0 for the next feature). The
  version in the panel header is the only reliable way to know which image
  booted after an OTA.
- One topic per commit, version in the commit title. `git log --oneline
  upstream/main..main` is the list of fork changes.
- When a patch is transferred by copy/paste, the last line (a single space,
  context for a blank line) gets trimmed and `git apply` reports
  "corrupt patch at line N". `scp` or `printf ' \n' >>` fixes it. Compare
  `wc -lc` and `md5sum` before applying.
- `git stash pop` does not restore the index. Build patches from commits
  (`git diff A B`), not from a stash round-trip.

### Upstream *(fork)*

```bash
git remote add upstream https://github.com/socquique/Monitor-SEN66.git
git fetch upstream
git checkout -b fix/<topic> upstream/main    # PR branches start here, not on main
```

PRs to upstream keep Spanish strings, his HA names, no version bump, no
dependency on fork-only code (battery saver, auth). Two are open: the touch
read abort and fan cleaning from idle.

### The upstream web installer

The upstream author publishes `flash.html` on GitHub Pages from a `gh-pages`
branch, by hand (no CI). **This fork does not publish there**; the notes are
kept in case it ever does:

1. Merge to `main` and bump `APP_VERSION`; the installer serves whatever
   `firmware/manifest.json` says.
2. The installer binary is NOT the OTA one: the manifest declares a single
   part at `"offset": 0`, i.e. a merged image (bootloader + partition table +
   app). `build/monitor_sen66.bin` is app-only; published at offset 0 it
   gives a device that does not boot. `idf.py merge-bin` is required.
3. Commit on `gh-pages`: the new `.bin` under `firmware/` and `manifest.json`
   with `version` and `path` updated.
4. The screenshots under `img/` go stale too; `index.html` states their count
   by hand. Regenerate with the simulator at panel size:
   `./build/sen66_sim --page N --warp 1 --offset 900 --scenario good --shots DIR 1`.

## Verified against hardware

### Upstream, 2026-08-20

First real boot: display, touch, RTC and SEN66 (serial 0123456789ABCDEF,
firmware 4.1) working on GPIO17/18. What was learned:

- **The LVGL buffer cannot be a fixed number of rows.** On this board the
  largest contiguous internal DMA block is **144 KB**, not Hamlet's ~192 KB,
  so the inherited 185 rows don't fit and `lvgl_port_add_disp()` fails in a
  bootloop. `display.c` now computes it at run time, reserving 56 KB for the
  rest of the system. Do not pin it by hand again.
- **Watch the free internal heap that `app_main` prints at the end.** With
  the buffer at maximum, 26 KB remained — very tight for WiFi station + MQTT
  + OTA. With 96 rows, 42 KB. If something has to give, it is the display:
  96 and 114 rows flush the same 5 chunks per frame.
- **The header silk screen beats the documentation.** See `board.h`.
- **Recover the I2C bus before opening it** (`bus_recover()` in `sen66.c`). An
  ESP32 reset mid-transaction leaves the SEN66 holding SDA low and the sensor
  shows as "not detected" until the next power cycle. Happens on every
  `idf.py flash`. Manual SCL pulses + STOP.
- **An optimistic log is not a check.** Twice in one session: `net.c` sang
  "portal open" with the radio off, and `webcfg.c` announced 192.168.4.1
  while on the home network. Log what was verified, not what was intended.

### Fork, 2026-09-12 (device: SEN66 fw 4.0, board with 1100 mAh cell)

- **The CST9217 NACKs one read during `phy_init`, reproducibly.**
  `esp_lvgl_port`'s `ESP_ERROR_CHECK` turned that into `abort()` on core 1.
  With rollback enabled, the first boot of every OTA image was reverted and
  the panel just kept showing the old version. Upstream esp-bsp #700 is the
  same failure; its fix (2.7.2) only covers the encoder trigger, the check is
  still in 2.9.0. Fixed by owning the indev in `display.c`.
- **Fan cleaning had never run.** `Start Fan Cleaning` is Idle-only per the
  datasheet; sent while measuring it is ACKed and ignored, so both the button
  and the weekly job "succeeded" silently. Now stop → clean → 11 s → start,
  like the recalibration. You can hear it.
- **`sen66_read()` already polls `Get Data Ready`**, so the 500 ms loop costs
  one 20 ms status query when nothing is new. Do not "optimise" it to 1000 ms:
  no power gain, and it aliases against the sensor's 1 Hz output.
- **Free internal heap after boot is ~30 KB** with the current buffer. Keep an
  eye on it before adding anything that allocates internal RAM.

## Decisions taken

- **Panel security** *(fork, supersedes upstream's "no password on purpose")*:
  no password by default, so the default experience is unchanged; optional
  Basic Auth; `Host` + `X-CSRF` guards on every API route; backup with
  passwords refused unless a panel password is set; WPA2 rescue portal that
  closes on reconnect; OTA rollback on; signed OTA prepared in
  `sdkconfig.defaults` but commented out because it needs a key.
- **The fan is NOT cycled on battery by default** — upstream's reasoning
  stands: stopped, nothing is measured. *(fork)*: it is available as an
  opt-in **battery saver** (default 180 s measuring every 600 s), only
  without USB. Trade-offs, from the datasheet: NOx needs ~5 min continuous
  and is blind in short windows; CO₂ ASC accuracy assumes continuous
  operation; PM settles ~30 s after each start; VOC state survives
  stop/start. Sensor draw ~90 mA measuring, ~3 mA idle. Measured autonomy
  with everything on: ~5–6 h on a 1000 mAh cell.
- **HA entity names follow Project Aura** (`Air Status`, `VOC Index`,
  `Battery Voltage`, …). `unique_id`s never change, so renames are safe for
  existing installations.

## Open

1. **Signed OTA**: generate the key, uncomment the four lines in
   `sdkconfig.defaults`, verify the build, then re-flash once over USB.
2. **Battery-saver measurement**: one run without and one with the saver,
   compare the `battery N% (mV)` log lines. The datasheet numbers are not
   yet confirmed on this device.
3. **Wiring photo** for the MakerWorld listing (upstream's item; the text was
   posted 2026-08-22).

## Done, in case you look for it

- **Sound-level meter (1.4.0).** The mics hang off an **ES7210 at 0x40**, not
  the ES8311. Hand-written driver (the official one uses the old I2C API).
  Three traps, one OTA each: probing with an invented ID register NACKs (use
  `i2c_master_probe`); the I2S channel is enabled BEFORE configuring the
  codec; and above all **in full-duplex the clocks are generated by the
  TRANSMIT channel**, which `sound.c` only switched on while an alert played
  — without MCLK the ES7210 delivers nothing. It now stays enabled and the
  amplifier is still switched off between alerts.
- **Noise is calibrated at a 112 dB offset** (2026-09-09), measured with a
  phone sound-meter app against the device and two anchors 10 dB apart:
  ambient 54.6 dB vs −57.8 dBFS, music 64.2 vs −48.1. That gives 112.4 and
  112.2, i.e. **a constant shift and not a slope**, which is the only thing a
  constant can correct. The factory 120 came from a comparison against a
  Qingping that **is worthless**: 1,983 samples give a correlation of 0.583,
  and in the final test the monitor rose 26 dB above its floor while the
  Qingping stayed at its 36 dB, alive. **Noise does not equalise in a room
  like CO₂ or temperature**, so two devices in different spots cannot be
  compared. Energy-average windows of tens of seconds on both sides at once:
  two instantaneous readings with music gave 95 and 111 depending on the
  second chosen.
- **The SEN66's fan adds 0.9 dB to the noise floor** (2026-09-09), i.e. the
  fan alone is at about **36 dB SPL**: real but fifteen dB below a normal
  room. Eight 45 s windows alternating, looking at the **minimum**: ON −68.0
  −68.0 −68.4 −69.1 −69.3 (mean −68.6) vs OFF −69.2 −69.6 −69.6 (mean
  −69.5). All three OFF below all five ON, with a quarter of the spread.
  **This corrects the August attempt**, which with short windows gave +5.2 dB
  and then −5.6 dB (impossible) and was closed as "not measurable": the
  method was failing, not the fan. What unlocked it: 45 s windows, the
  minimum or 5th percentile as the statistic, and **alternating A/B/A/B
  instead of measuring A once and B once**.
- **The published level is instantaneous** (125 ms RMS), not an average, **the
  device floor is 43 dB** and **there is no A-weighting**: raw RMS. The three
  together explain 53 dB here vs 37 on a consumer meter with neither wrong.
  The A curve removes mainly lows, which is all that is left in a quiet room,
  so the difference is large at the bottom and small with music — hence
  agreement with the Qingping within 2 dB at 75 dB. **How much is floor and
  how much is weighting cannot be told** from the data: a phone app gives a
  28 dB minimum where the monitor does not go below 43. Practical: below ~50
  dB the number is not meaningful. If the low range ever matters, the step is
  A-weighting in `mic.c`.
- **The AirRing enclosure's airflow is verified** (2026-08-22), not assumed.
  The SEN66 has two inlets (square opening and membrane) and one outlet (the
  fan), and Sensirion asks to separate them from each other and isolate them
  from the device's interior. Both checked by measurement: +0.4 °C against an
  independent thermometer rules out drawing from inside the case, and an
  incense test gives a PM2.5 decay constant of **9.7 min vs 8.7** for the
  reference sensor, ruling out re-inhaling its own outlet. Raw data on the
  upstream author's DietPi (`/root/prueba-humo-20260822.csv`, not in the repo).
- **NO offset is applied, neither temperature nor CO₂, and that is a measured
  decision** (2026-08-22): 22 h of logging against a Qingping Air Monitor 2
  plus an independent thermometer, all three on the same table. The full
  reasoning is in the README under "On measuring the deviation"; raw data on
  the upstream author's DietPi (`/root/comparacion-20260821.csv`). In short:
  - Against the reference the SEN66 is at **+0.4 °C**, within its own
    tolerance (±0.5 °C). The humidity deviation we saw was **the Qingping's**
    (+8.9 %), not the SEN66's.
  - The temperature difference **is not constant**: +0.20 to +1.30 °C by time
    of day. A fixed offset is right at one hour and wrong at another.
  - In CO₂ the discrepancy is **slope, not zero**:
    `SEN66 = 1.20 × Qingping − 138`, crossing at 675 ppm. Below it the SEN66
    reads less, above it more.
  - CO₂ ASC **is fine**: one afternoon's 403 ppm looked like a pinned zero,
    but the night curve (403 → 831 at dawn) proves the afternoon really was
    ventilated.
- **Three method lessons, all paid for in one session**: comparing two
  devices gives the difference and never who is right; measure a full 24 h
  cycle because a "constant" difference usually is only by day; and check at
  several concentrations, because a single range turns a slope difference
  into a false zero error.
- **CO₂ alarm and forced recalibration** reached the web panel in 1.2.0. The
  recalibration does not run in the server thread: the command requires the
  measurement stopped, so the panel queues it and `sensor_task` runs it. Two
  details from Sensirion's documentation that are not deducible from code:
  the returned correction carries an **offset of 0x8000**, and you must wait
  **600 ms between stopping and recalibrating**.
- **The recalibration path is not tested against hardware**: firing it writes
  the sensor's EEPROM and only makes sense outdoors at 420 ppm. Validation,
  queuing and rejection of impossible references are tested.
- **Never edit mqttthing accessories from the Homebridge form**: it deletes
  the whole `topics` block because it cannot represent `apply` functions. It
  happened, and left all four accessories reading nothing. See
  `docs/HOMEBRIDGE.md`.
- **The level that goes over MQTT is an English token** (`good`…`bad`), not
  the translated string. Changing it broke the Homebridge mapping and the
  README was left saying the old thing.
- **Fork, 1.8.0–1.8.5** (2026-09-12): security hardening + German panel +
  Aura names (1.8.0); battery saver (1.8.1); tolerant touch read (1.8.2);
  English logs (1.8.3); fan cleaning from idle (1.8.4); English battery
  entity names (1.8.5). Docs and README in English. Every step went onto the
  device over OTA; the 1.8.1 rollback is what surfaced the touch bug.
