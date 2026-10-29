"""Decodes a capture of the Geco GH11TB controller's RS485 bus (sniff.ps1, 9600 8N1) - docs/geco-gh11tb-bus.md.

Every byte on the wire is bit-inverted. Inverted, the controller repeats this about every 57 ms, also with no room
panel attached:
  40 T1 T2 T3 fa 00 fa 00 fa CK | ff xx 23      temperatures x 2: T1 boiler, T2 feeder, T3 hot water (DHW)
  50 fa fa P3 00 00 OUT MODE 00 CK | ff xx 23   P3 0x40 = feeder piston away from home; OUT 0x40 fan, 0x20 feeder,
                                                0x02 CO pump; MODE 0x04 standby, 0x08 burning, 0x10 podtrzymanie
  60 .. | ff xx 23,  70 .. .. | ff xx 23,  72 .. .. | xx 23   not decoded yet
CK = sum(bytes 0..8) - 0x10 (mod 256). Every piece ends with 0x23 (0xdc on the wire).

Usage: python -I decode.py <capture.txt> [--all]
  default: every change of the controller's state or temperatures (a temperature once it held for 10 s)
  --all:   every frame that passes its checksum
"""

import datetime
import re
import sys

MODES = {0x04: 'standby', 0x08: 'burning', 0x10: 'podtrzymanie'}
OUTPUTS = ((0x40, 'fan'), (0x20, 'feeder'), (0x02, 'CO pump'))


def checked(f):
    return len(f) >= 10 and (sum(f[:9]) - 0x10) & 0xFF == f[9]


def describe(f):
    if f[0] == 0x40:
        return f'boiler {f[1] / 2:.1f} C  feeder {f[2] / 2:.1f} C  DHW {f[3] / 2:.1f} C  rest {f[4:9].hex(" ")}'
    outs = [name for bit, name in OUTPUTS if f[6] & bit]
    unknown = f[6] & ~sum(bit for bit, _ in OUTPUTS)
    return (f'{MODES.get(f[7], f"mode 0x{f[7]:02x}")}: {", ".join(outs) or "all off"}'
            + (' | piston away' if f[3] & 0x40 else '') + (f' | unknown output bits 0x{unknown:02x}' if unknown else '')
            + f'  [{f[:10].hex(" ")}]')


def frames(path):
    """(Unix time, inverted frame) of every 0x40 / 0x50 frame that passes its checksum, plus the count that did not."""
    with open(path, encoding='utf-8') as fh:
        head = fh.readline()
        started = re.search(r'started (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)', head).group(1)
        t0 = datetime.datetime.strptime(started, '%Y-%m-%d %H:%M:%S').timestamp()
        buf, bad, out = bytearray(), 0, []
        for line in fh:
            if not line.strip():
                continue
            ms, *hexes = line.split()
            buf += bytes(int(h, 16) ^ 0xFF for h in hexes)
            while (end := buf.find(0x23)) >= 0:
                piece, buf = bytes(buf[:end + 1]), buf[end + 1:]
                if piece[:1] in (b'\x40', b'\x50'):
                    if checked(piece):
                        out.append((t0 + int(ms) / 1000, piece[:10]))
                    else:
                        bad += 1
    return out, bad


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    found, bad = frames(sys.argv[1])
    print(f'{len(found)} frames passed the checksum, {bad} did not')
    show_all = '--all' in sys.argv
    last, pending = {}, {}
    for t, f in found:
        stamp = datetime.datetime.fromtimestamp(t).strftime('%H:%M:%S')
        if show_all:
            print(stamp, describe(f))
            continue
        key = f[0]
        if last.get(key) == f:
            pending.pop(key, None)
            continue
        if key == 0x40 and key in last:              # a temperature at a degree's edge flickers: wait 10 s
            first = pending.get(key)
            if not first or first[0] != f:
                pending[key] = (f, t)
                continue
            if t - first[1] < 10:
                continue
            pending.pop(key)
        print(stamp, describe(f))
        last[key] = f


if __name__ == '__main__':
    main()
