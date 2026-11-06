# Geco GH11TB - the room-panel bus, read without sending

Worked out on 2026-10-29 at dad's boiler (Spyra KWKMP E-plus, controller Geco GH11TB, board TKT-MGH11TB-AA-5, code
MGH11-AA-08) by listening only - nothing was ever sent to the controller. Tools: `diagnostics/geco/sniff.ps1`
(records), `diagnostics/geco/decode.py` (decodes); a 55 s sample is in
`diagnostics/geco/sample-9600-automat-start.txt`.

## Wiring

The panel terminals of the GH11TB (manual p. 16 and 18): **19 +12V, 20 GND, 21 A, 22 B**. The manual's own drawing
marks 22 as "+" and 21 as "-", so a Waveshare-style adapter goes **A+ -> 22, B- -> 21**; swapping them only garbles
the bytes. 20 (GND) may be added; **19 (+12 V) never**. The controller must be switched off and unplugged while its
cover is open.

**After every power cut the controller comes back in standby, not in automat** - the A button must be pressed, or
the fire goes out (it sat like that for 40 minutes on the day of this test).

## Line

- **9600 baud, 8N1.** Fewest framing errors by far; even/odd parity and 4800/19200 give ten times more. Not the
  Hewalex/GECO 38400 protocol, not Modbus RTU (no frame has a valid Modbus or CCITT CRC).
- The controller talks **with no room panel attached**: about every 57 ms, two bursts separated by ~10.7 ms of
  silence, ~42 bytes in all.
- **Every byte is bit-inverted.** All of the below is after inverting (`x ^ 0xFF`). Every piece ends with `0x23`
  (`0xdc` on the wire).
- Stray `ff`/`00` bytes appear at frame boundaries (the line turning around); the checksum filters them out.

## Frames

```
40 T1 T2 T3 fa 00 fa 00 fa CK | ff xx 23        temperatures
50 fa fa P3 00 00 OUT MODE 00 CK | ff xx 23     state
60 .. | ff xx 23,   70 .. .. | ff xx 23,   72 .. .. | xx 23        not decoded yet
```

**Checksum** of the 10-byte frames: `CK = sum(bytes 0..8) - 0x10` (mod 256). It held for every frame seen - 8281 of
8281 in the longest recording.

**`0x40` - temperatures**, each **value / 2 = degrees C**, checked against the display and our own probes:

| Byte | Meaning | Seen |
|---|---|---|
| T1 | boiler (T1) | 0x84 = 66 C - the display showed 66 |
| T2 | feeder (T2) | 0x50 = 40 C - our hopper probe 40.3 |
| T3 | hot water (T3) | 0x74 = 58 C - our boiler probe 58.5 |
| `fa` fields | probably sensors that are not fitted (return T8 etc.) | constant |

**`0x50` - state:**

| Byte | Bits | Meaning |
|---|---|---|
| OUT (byte 6) | `0x40` | fan running |
|  | `0x20` | feeder running |
|  | `0x02` | CO pump running (the vertical bar left of the display) |
|  | `0x08` | hot water (CWU) pump running: the return jumps to 56-59 C, T1 is pulled down, another burn follows |
| MODE (byte 7) | `0x02` | service menu: everything off, the bus drops out for a moment now and then; leaves through standby |
|  | `0x04` | standby - switched on, not in automat |
|  | `0x08` | automat, **burning** |
|  | `0x10` | **podtrzymanie** (holding: boiler at or above u0) |
| P3 (byte 3) | `0x40` | feeder piston away from its home position (the kontaktron). Home is pushed in, closing the hopper's outlet (seen at the boiler, 2026-11-06); the bus itself does not show the direction |
|  | `0x08` | always set |
|  | `0x04` | seen once, 04.11 21:52-22:30 after a bath, 47 min - meaning unknown |

Still unknown: the short `60/70/72` frames (settings? alarms?); the `ff xx 23` tails.

## What the controller does, as seen on the bus

- **Pressing A at 66 C** (above u0 = 65): straight to podtrzymanie; fan and feeder run ~30 s while the piston goes
  home, then stop. The CO pump runs.
- **Przedmuch** in podtrzymanie: exactly **u2 = 40 min** after it began - fan + feeder on, the piston leaves home
  after 4 s, the feeder stops when it is back home (**31 s**), the fan **5 s later** (service parameter F18).
- **Back to burning** at u0 - hysteresis = 63 C: mode `0x08`, the fan runs all the time, the feeder gives **portions
  of 1-2 s every u1 = 18 s** (the "equal portions" algorithm, service parameter F21) while the piston advances.
- The fan and feeder keys in standby switch those outputs on directly (seen as the OUT bits for ~2 s each). Pressing P
  (parameter menu) does not show on the bus.

## Writing (u0, u4)

Not possible yet: the controller only announces its state; what the GA01HA panel sends back (its setpoints) has
never been on this bus. The safe way is to listen to a real GA01HA once while changing u0 and u4, then let the ESP
answer the same way. Blind writes are out - an unknown frame could change something else in the controller. The
panel can set only u0 and u4 anyway; service parameters (fan, feeder, F18) stay on the controller's keypad.

## In the firmware (v28, S3 only)

`GecoBus.h` listens through an RS485 breakout (pins RO, /RE DE, DI, G, VIO, 5V; terminal G, B/-, A/+; switches
PUPD and 120 ohm) on the same terminals:

| Module | S3 / controller |
|---|---|
| RO (what the module receives) | GPIO 18 |
| /RE DE | GND - receiver on, driver off, so it never drives the line |
| DI | not connected |
| G | GND |
| VIO | 3V3 |
| 5V | 3V3 too, bridged to VIO on the module (the module is rated for 3.3 V). Not the S3's 5V pin: it is an input only ("5V in") - wired there the transceiver got 0.69 V and heard nothing |
| A/+ / B/- | terminal 22 / 21; the module's own G not connected |
| PUPD (bias) | on |
| 120 ohm | off |

The UART is opened without a TX pin, so the ESP cannot send. Until the first good frame the firmware checks the
connection by itself (v29): every 30 s (5 min after the first 10) it reads GPIO 18 with the pull-up and then the
pull-down - toggling = data, held high/low = the module drives it (an idle line, or the transceiver unpowered while
VIO keeps its RO side alive), following the pull = nothing drives it (no VIO, or RO on another pin). Data without
frames flips the UART's RX inversion (A and B swapped); a
silent pin makes it look for the bus on the free header pins (`GECO_SCAN_PINS`, no strapping pins) and listen
there. `/api/now` -> `geco`: `bytes` (received at all), `pin`, `inv`, `line` ('d' / 'h' / 'l' / 'f'), `raw` (the
last 16 bytes as received). On 2026-10-30 the module, its 5V pin on the S3's 5V pin, gave no byte in 30 minutes
(line 'h'); on the bench (2026-11-02, the ESP driving A/B itself) the same module decoded every frame as soon as
its 5V pin went to 3V3.

What it hears:

- `/api/now` -> `geco`: seconds since the last good frame (`age`, -1 = never heard), `mode`, `out`, `flags` (P3), `since`
  (the last change of any of them - not when the mode began; a burn changes it every portion), `t` (the controller's
  three temperatures), good/bad frame counts.
- `/api/geco?from&to`: the work log ("/gw" on flash, 390 000 changes = 3 MB: about 2 months of winter burning, 5 of
  autumn - burning makes ~6 changes a minute, the feeder's 1-1.5 s portions every ~19.5 s; holding almost none) - 8
  bytes a change (time u32, tenths, mode, outputs, P3), the first record being the state the range began in; changes
  not on flash yet are served too. Every start closes the log at the last "still alive" mark before it (a NO_DATA
  record), so the state before a power cut or an update does not run on through it (v30).
- `/api/work?from&to`: the same work hour by hour ("/wk2", 30 bytes an hour, kept 2.5-5 years; the hours of v30 and v31
  are counted anew from the log), worked out from the log - seconds heard, burning, holding, standby, fan, CO pump, hot
  water pump, the feeder in tenths and its part while burning (v32: with the burn's end - the stroke finishing and the
  fan's after-run, in holding right after a burn), the heat model's degree-minutes while heard, the flue 4 minutes into
  each burn begun without the hot water pump (between its two minute readings, +-16 C between burns; sum + count), burns
  begun (v32: coming back from the service menu into a burn is none), piston cycles completed (away -> home) and
  przedmuchy begun (a burn's end is none); the hour running now last, its part so far. Written 6 minutes after each hour
  ends, so the last burn's flue reading is there.
- Settings read off the work (v31, `SettingWatch`; user parameters never appear on the bus): u1 = the feeder's pause while
  burning, u2 = from the feeder stopping in holding to the next przedmuch, the fan's after-run (F18) = how long it
  outlasts the feeder in holding. `/api/now` -> `geco.set` [u1 s, u2 min, after-run s]; each change and each visit to the
  service menu goes into the journal (`EV_SETTING`, 16: sensor 0-2 the setting, value old x 256 + new; sensor 3 the menu,
  value seconds), no notification. u3 (fan power) and the service parameters stay invisible - a visit to the menu is the
  only trace.
- The controller's temperatures as three more probes (address `F7 0n ...`, n = 1 boiler, 2 feeder, 3 hot water), added
  by themselves the first time the bus is heard. The app pairs each with our probe on the same water (boiler ->
  furnace probe, feeder -> hopper, hot water -> boiler): its value on that probe's card, a dashed second line on its
  chart (off by default, the "Sterownik" chip; drawn anyway while our probe is silent), a banner when the two differ
  by more than 5 degrees for 15 minutes, and a one-tap auto-correction of our offsets (Ustawienia -> Korekta wskazan:
  the middle 60 % of two hours of minute differences, shown before it is saved).
- Warning 6 "Sterownik nie w automacie" (`WARN_STANDBY`, setting `standbyOn`): mode standby for 2 minutes while the
  bus is heard, cleared after 1 minute in automat; pushed like every warning. While the bus is heard it replaces the
  temperature guess of "Piec stoi po zaniku pradu".
- Coal from the feeder (v30): coal leaves the hopper only through the feeder, so while the bus is heard coal = kg per
  feeder second (`coal.kf`) x the feeder's seconds, the heat model only for the time it is not. `kf` is learnt like
  the heat factor, from intervals between refills to the top in which the controller was heard at least 70 % of the
  time (and the feeder ran 10 minutes or more); until two such refills it is scaled from the heat model over the
  last two weeks the bus was heard (`coal.kfN` = 0). Measured 2026-11-04: a full piston cycle is ~31 s of the motor.
- The app: an animated boiler on the home screen (with the hopper's coal level). `/api/now` alone cannot drive it, a
  refresh every 15 s catches one feeder portion in five, so since v32 it plays `/api/geco`: the last 6 hours, topped up at
  each refresh (a few dozen bytes, ~260 ms), shown 18 s behind the device. The piston follows the motor's seconds since it
  last came home (a crank, ~30.3 s to a cycle: back for the first half, forward for the second); "od" is when the state really began, from the log; a burn's end has its
  own name, "dobieg". Without the log it falls back on the snapshot. Work lanes under the charts (from
  the log up to 8 days, from the hours beyond; the mode that held most of a column, with a thin line at the lane's foot
  for what was too short to lead it), the controller's day on "Dzisiaj" (burning, holding, przedmuchy,
  piston cycles, feeder time), its month in Miesiace, and "Praca sterownika" in Wykresy: per day for 30 days how many
  times the fire was started (burns + przedmuchy), the share of the coal fed outside burning and the flue 4 minutes into
  a burn, the days the settings changed marked - what a test of settings is read from. Whether the flue mark really
  follows soot on the exchanger is still to be seen.

## Next

Proposals agreed on 2026-10-29, after the first S3 data:

- Map the short frames from what the S3 logs.
- "Coal hung or ran out": burning, the feeder pushing, and the boiler not warming for 30-45 minutes - coal bridged in
  the hopper or the hopper empty. (It also makes the feeder count too much coal for that time.)
- The controller's switch points on the furnace chart: u0 (to podtrzymanie) and u0 - hysteresis (back to burning),
  read from T1 at the mode changes in the log.
- Bus silence: no frame for over 2 minutes while the ESP runs - the controller is off or a wire came loose.
