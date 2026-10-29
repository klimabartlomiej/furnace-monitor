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
| MODE (byte 7) | `0x04` | standby - switched on, not in automat |
|  | `0x08` | automat, **burning** |
|  | `0x10` | **podtrzymanie** (holding: boiler at or above u0) |
| P3 (byte 3) | `0x40` | feeder piston away from its home position (the kontaktron) |
|  | `0x08` | always set |

Still unknown: the DHW (boiler) pump bit - it comes on only when the hot water falls below u4; the short `60/70/72`
frames (settings? alarms?); the `ff xx 23` tails.

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

## Next

- The S3 listens through a TTL-RS485 module on the same terminals, with the module's transmit input left
  unconnected (it cannot send), and shows the controller's state and temperatures in the app; a notification when
  the controller is not in automat (after a power cut).
- Map the DHW pump bit and the short frames from what the S3 logs.
