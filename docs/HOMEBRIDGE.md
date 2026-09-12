# Homebridge (HomeKit) without Home Assistant

The device publishes over MQTT exactly as it does for Home Assistant; what
changes is who listens. With
[`homebridge-mqttthing`](https://github.com/arachnetech/homebridge-mqttthing)
it ends up in Apple's **Home** app and in Siri.

Ready-to-paste configuration: [`homebridge.json`](homebridge.json). The
`apply` functions are tested against the device's real JSON.

## Set-up

1. **Broker**: set it up as described in [MQTT.md](MQTT.md). With Homebridge,
   leave the **discovery prefix empty** in the device's panel: auto-discovery
   is only understood by Home Assistant, and otherwise a dozen retained
   messages sit in the broker with nobody consuming them.

2. **Plugin**: from the Homebridge UI, or from the console if it is the
   official install with its own Node in `/opt/homebridge`:

   ```bash
   hb-service add homebridge-mqttthing
   ```

3. **Device**: in its web panel, broker `mqtt://YOUR-DIETPI-IP:1883` with that
   user and password. It reboots on save.

4. **Homebridge**: paste the two accessories from `homebridge.json` into the
   `accessories` array of your `config.json` and restart.

> **Edit these accessories ONLY as JSON.** The Homebridge UI form cannot
> represent topics with an `apply` function, and saving from there — even if
> you only change the name — wipes out the whole `topics` block. The
> accessory still shows up in HomeKit but no longer reads anything, and the
> log fills with errors like `Cannot read properties of undefined (reading
> 'getAirQuality')`. Use the UI's JSON editor (Config > JSON) or the file
> directly.

## What you see and what you don't

| Metric | In HomeKit |
|---|---|
| Overall level | Air quality, 1–5 scale (1:1 mapping with the firmware's five levels) |
| PM2.5, PM10 | Densities, in the accessory's detail view |
| CO2 | Level in ppm + a CO2 accessory that alerts above 1200 ppm |
| Temperature, humidity | Services inside the same accessory |
| **VOC and NOx** | **Not shown** |
| Battery | Level, charging state and low-battery alert, if there is a cell |
| **Noise** | **Not as a number**: HomeKit has no sound sensor. It goes as a "Loud" binary |

VOC and NOx are left out on purpose: HomeKit expects densities in µg/m³ and
the SEN66 gives dimensionless *indices* (1–500). Publishing them as if they
were a density would be inventing the unit. They are still visible on the
device's screen and in its web panel.

**Exception, if you want graphs**: the Home app keeps no history, but the
**Eve** app does, and for that mqttthing requires `getVOCDensity`. If it is
worth it to you, add to the air-quality accessory:

```json
"getVOCDensity": {
  "topic": "sen66-XXXXXX/state",
  "apply": "const v=JSON.parse(message).voc; return v==null?state:v;"
},
```

plus `"history": true` — knowing that this number is an index dressed up as
µg/m³.

## Noise

HomeKit **has no sound-level service whatsoever**, and mqttthing does not
invent one: its sensor types are air quality, CO2, CO, contact, humidity,
leak, light, motion, occupancy, smoke, temperature and pressure. Full stop.

So the number does not fit. What does fit, and is useful, is a **threshold**:
an occupancy sensor called "Loud" that trips above 65 dB, and with that you
can already build automations ("if it is loud and nobody is home, notify
me"). The threshold is changed in the accessory's own `apply`.

You could smuggle the level in as a light sensor, which is the hack that
circulates, and the Home app would show "34 lx". It is not done, for the same
reason VOC and NOx are left out: inventing a unit for a reading is worse than
not showing it.

## Battery

Homebridge **does not use auto-discovery**, so the battery does not appear by
itself as in Home Assistant: you have to give it the topics, and they are
already in `homebridge.json`. mqttthing adds a **battery service** to any
accessory as soon as it sees `getBatteryLevel`, `getChargingState` or
`getStatusLowBattery`.

They go only on the air-quality accessory, not on the CO2 one: on both, the
Home app would show two batteries for the same device.

If the device **has no cell**, those fields are absent from the JSON and the
`apply` functions return the previous value, so they do no harm; but if you
are never going to fit a battery, the clean thing is to remove the three
topics.

In the Home app the battery is not a separate icon: it shows **inside the
accessory**, in its settings. And if you have just added it, the app may take
a while to notice that the accessory has a new service; force-quitting the
app is usually enough.

## The Node warning

Homebridge warns that the plugin wants Node 18/20/22 and you have 24:

```
The plugin "homebridge-mqttthing" requires a Node.js version of
^18.12.0 || ^20.10.0 || ^22.11.0 which does not satisfy the current
Node.js version of v24.19.0
```

**It is a warning, not a failure, and it can be ignored.** There is no
version that fixes it: 1.1.49 is from January 2026 and still declares 22 as
the maximum. Checked with `logMqtt` enabled on Node 24.19.0 and Homebridge
2.4.0: it receives the messages every 10 s, the `apply` functions decode them
and the characteristics update.

```
[CO2] Received MQTT: sen66-XXXXXX/state = {"co2":470,...}
[CO2] apply() function decoded message to [NORMAL]
[CO2] apply() function decoded message to [470]
```

Downgrading Node to 22 to silence the warning would drag along every other
plugin, which currently work on 24: not worth it. That said, the plugin
author has not tested on 24, so if the plugin ever does something odd, this
is the first thing to look at.

## Notes

- The 1200 ppm threshold of the CO2 alert is in the `apply`, not in the
  firmware: change it in `config.json` and restart Homebridge.
- The firmware publishes the Home Assistant auto-discovery messages under
  `homeassistant/...` regardless. Without HA they sit there retained doing no
  harm, and if you ever add HA the device appears by itself. To not publish
  them, **empty the prefix** in the web panel.
- The state topic is published **retained**, so when Homebridge restarts the
  accessories get the last value instantly. Without that, mqttthing starts
  up warning of `characteristic value ... received "undefined"` until the
  next publish arrives.
- The device **depends on none of this**: the screen and its web panel work
  even with the DietPi switched off.
