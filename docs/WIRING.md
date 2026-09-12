# Wiring the SEN66

Four wires. The sensor ships with the **six**-conductor JST GH lead, but two
are unused: the SEN66 has no interface-select pin (that was the SEN5x) and
speaks I2C directly.

```
 Board header, SEEN FROM THE BACK with the USB-C on the right.
 Every pin has its name printed next to it: wire by LABEL,
 not by counting pins.

  ┌──────┬──────┬──────┬─────┬─────┬─────┬─────┬──────┐
  │ IO18 │ IO17 │ IO16 │ RXD │ TXD │ 3V3 │ GND │ VBUS │
  └──┬───┴──┬───┴──────┴─────┴─────┴──┬──┴──┬──┴──────┘
     │      │                         │     │        ^^^^
  yellow  green                     red   black    don't touch
     SCL    SDA                      VDD    GND       (5 V)
      │      │                         │     │
      └──────┴────────► SEN66 ◄────────┴─────┘

 SEN66 connector (6-pin JST GH), cable leaving upwards:
 pin 1 is the RED-wire end.

  1 red VDD · 2 black GND · 3 green SDA · 4 yellow SCL
  5 blue NC · 6 purple NC          (5 and 6 unconnected)
```

## Table

| SEN66 pin | Wire colour | → | Label on the board |
|---|---|---|---|
| 1 VDD | red | → | **3V3** |
| 2 GND | black | → | **GND** |
| 3 SDA | green | → | **IO17** |
| 4 SCL | yellow | → | **IO18** |
| 5 NC | blue | | *unconnected* (internally tied to GND) |
| 6 NC | purple | | *unconnected* (internally tied to VDD) |

**How to orient the sensor connector**: with the cable leaving upwards, pin 1
is the **red-wire** end, and the order is red, black, green, yellow, blue,
purple. Cut the blue and purple wires or insulate them with heat-shrink: they
are internally tied to GND and VDD and add nothing.

**Do not trust the header's pin numbers.** The board's silk screen reads
`IO18 IO17 IO16 RXD TXD 3V3 GND VBUS` from left to right (from the back,
USB-C on the right), which is NOT the order given by the 1..8 numbering in
the official `HARDWARE_REFERENCE.md`. What is printed on the copper wins.

## Warnings that matter

- **To 3V3, never to VBUS.** The SEN66 wants **3.3 V ±5 %** — a narrow
  margin. VBUS (the USB's 5 V) is the **rightmost** pin, next to GND and two
  positions from 3V3: it is the easy mistake to make. The board's GPIOs
  **do not tolerate 5 V**.
- **Current: peaks up to 350 mA.** That is Sensirion's warning in their own
  driver, and Waveshare's documentation does not say how much current the
  header's 3V3 can supply. In practice the AXP2101's 3.3 V buck has plenty of
  headroom, but **power the board from a USB-C charger rated 1 A or more**,
  not from a hub port. Symptoms of not enough: brownout resets, CRC errors
  on the I2C bus, or `fan_error` in the sensor status. Fix in that case: a
  separate 3.3 V supply for the sensor, common GND.
- **The SEN66's I2C address is `0x6B`, the same as the on-board QMI8658
  IMU.** That is why the sensor goes to GPIO17/18 on a separate I2C bus
  (port 1) and not on the main bus (GPIO14/15): on the same bus they would
  collide.
- **Beware of GPIOs that look free but aren't**: GPIO13 is the display's
  LCD_TE signal and GPIO21 is the IMU's INT2 interrupt. The only truly free
  ones on the header are **16, 17 and 18** (plus UART0 on 43/44, if you do
  not need the serial console).
- **Short wires.** The bus runs at 100 kHz on the ESP32-S3's internal
  pull-ups (~45 kΩ, weak). With 10–15 cm of Dupont wire it works; if you
  lengthen them a lot and CRC errors appear, add external 4.7 kΩ pull-ups to
  3V3 on SDA and SCL.

## Check at boot

If something is not where it should be, the firmware says so on the console
(`idf.py monitor`):

```
I (1500) sen66: product: 'SEN66'
I (1500) sen66: ready at SDA=17 SCL=18
I (1520) app: SEN66 serial 0123456789ABCDEF, firmware 4.0
```

If it does not answer on GPIO17/18, it scans the other header pairs and
reports which one works. And if it shows up on none, it is wiring: check 3V3,
GND, and that the wires are not crossed.

## Sources

- [Official board hardware reference](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/HARDWARE_REFERENCE.md)
  (header H2 and full GPIO map).
- [Official SEN66 driver](https://github.com/Sensirion/arduino-i2c-sen66)
  (sensor pinout, wire colours, voltage and the 350 mA warning).
