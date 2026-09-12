# The MQTT broker

The device does not talk to Home Assistant or HomeKit directly: it publishes
over MQTT and they listen. So **you need a broker**, and it is the same step
whether you go on to [Home Assistant](../README.md#home-assistant) or to
[Homebridge](HOMEBRIDGE.md). If you already have one, skip step 1.

## 1. Install Mosquitto

### If Home Assistant runs as HAOS or supervised

Settings > Add-ons > Store, install **Mosquitto broker** and start it. Create
the user under Settings > People > Users and use it in step 3; the add-on
accepts HA credentials. That's it: HA detects its own broker and offers to
set up the MQTT integration by itself.

### If it is a bare Debian (DietPi, Raspberry Pi OS, a container)

`dietpi-software` does NOT have it in its catalogue (checked on DietPi
10.6): it goes through apt.

```bash
apt-get install -y mosquitto mosquitto-clients
```

Mosquitto 2.x out of the box **only listens on localhost and refuses
anonymous connections**. Freshly installed, the monitor will not connect and
will not say why, so open it up:

```bash
cat > /etc/mosquitto/conf.d/local.conf <<'CONF'
listener 1883 0.0.0.0
allow_anonymous false
password_file /etc/mosquitto/passwd
CONF
mosquitto_passwd -c /etc/mosquitto/passwd monitor
chown mosquitto:mosquitto /etc/mosquitto/passwd
chmod 0600 /etc/mosquitto/passwd
systemctl restart mosquitto
```

Two stones you will definitely trip over:

- **Do not repeat `persistence` or `persistence_location`** in `conf.d`:
  they are already in the Debian package's `mosquitto.conf` and the service
  refuses to start with `Duplicate persistence_location value`.
- **The password file must be `mosquitto:mosquitto` and 0600.** With
  `root:root` the broker exits with status 13 (permission denied). It is
  confusing that `mosquitto_passwd`, running as root, warns of exactly the
  opposite: the broker, which runs as `mosquitto`, is the one that counts.

Check that it really started — `restart` does not complain even if it fails:

```bash
systemctl is-active mosquitto && ss -lntp | grep 1883
```

## 2. Decide on the discovery prefix

In the device's panel, the **discovery prefix** field decides whether it
announces itself:

| Value | Effect |
|---|---|
| `homeassistant` (default) | Publishes auto-discovery and HA creates the device with its entities |
| empty | No discovery published. This is what you want **if you use Homebridge** and not HA |

Leaving it empty with Homebridge is not cosmetic: otherwise a dozen retained
configuration messages sit in the broker with nobody consuming them.

## 3. Point the device at the broker

In its web panel (the IP shown on the screen), under Home Assistant and
HomeKit:

- **MQTT broker**: `mqtt://BROKER-IP:1883`
- **User** and **password**: the ones from step 1
- **Discovery prefix**: per the table above

On save it reboots and connects. The screen shows the network state inside
the ring.

## 4. Check that it publishes

From the broker machine, before touching HA or Homebridge. The state is
retained, so it must arrive **instantly**; if it takes time, it is not
publishing:

```bash
mosquitto_sub -h localhost -u monitor -P 'YOUR-PASSWORD' -t '#' -v -C 5
```

With `#` you get everything in the broker; the two that matter are these,
with `sen66-xxxxxx` derived from the MAC (the last six digits, the same as in
the portal name `SEN66-XXXXXX`):

```
sen66-8625d4/status  online
sen66-8625d4/state   {"pm1":2.8,"pm25":3.4,...,"co2":563,"level":"good","rssi":-66}
```

- `state`: one JSON every 10 s with all metrics, the overall level and the
  WiFi signal.
- `status`: `online` / `offline`, with a *last will*. That is what makes HA
  mark the device unavailable if it goes down, instead of leaving the last
  value frozen and passing for current.

Careful if you want to filter by topic instead of listening to everything:
MQTT's `+` wildcard takes **a whole level**, so `sen66-+/state` matches
nothing. Either use the full identifier, `sen66-8625d4/#`, or listen on `#`.

The `level` field is a **stable English identifier** (`good`, `fair`,
`moderate`, `poor`, `bad`) that does not change with the display language,
so automations do not break when you change it.

To see the discovery, if you have it enabled:

```bash
mosquitto_sub -h localhost -u monitor -P 'YOUR-PASSWORD' -t 'homeassistant/#' -v -C 12
```

## If it does not connect

| Symptom | Usual cause |
|---|---|
| The device says it cannot connect and the broker logs nothing | Mosquitto is still localhost-only: the `listener` from step 1 is missing |
| The broker log says `Connection refused: not authorised` | Wrong user or password, or `password_file` missing |
| Connects and drops every few seconds | Two clients with the same *client id*: they kick each other out |
| HA does not create the device | Empty prefix, or the MQTT integration is not added in HA |
| Values stay frozen | Look at `status`: if it says `offline`, it is the device that went down |

To see what is happening in the broker:

```bash
journalctl -u mosquitto -n 50 --no-pager
```

## Clearing retained discovery

If you change the prefix or retire the device, the configuration messages
stay in the broker because they are retained, and HA will keep resurrecting
the ghost device. Clear them by publishing an empty retained message on each
topic:

```bash
for k in pm1 pm25 pm4 pm10 temperature humidity voc nox co2 noise level rssi; do
  mosquitto_pub -h localhost -u monitor -P 'YOUR-PASSWORD' -r -n \
    -t "homeassistant/sensor/sen66-xxxxxx/$k/config"
done
```

With a battery fitted there are four more: `sensor/sen66-xxxxxx/battery`,
`sensor/sen66-xxxxxx/bat_mv`, `binary_sensor/sen66-xxxxxx/charging` and
`binary_sensor/sen66-xxxxxx/usb`, all under the same prefix.
`mosquitto_sub -t 'homeassistant/#'` shows you the exact topics to clear.
