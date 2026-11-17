#!/usr/bin/env python3
"""Parse 01_XG.mid into a time-ordered event list for the MU50 host renderer.
Output: text lines: <frame> SHORT <status> <data1> <data2>   (48 kHz frames)
                  <frame> SYSEX <hex bytes>
Tempo map is honoured (meta 0x51). Only events up to --seconds are emitted.
"""
import struct, sys, argparse

def parse_vlq(data, i):
    v = 0
    while True:
        b = data[i]; i += 1
        v = (v << 7) | (b & 0x7f)
        if not (b & 0x80):
            break
    return v, i

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('midifile')
    ap.add_argument('outfile')
    ap.add_argument('--seconds', type=float, default=30.0)
    ap.add_argument('--rate', type=int, default=48000)
    args = ap.parse_args()

    with open(args.midifile, 'rb') as f:
        data = f.read()
    fmt, ntrk, div = struct.unpack('>HHH', data[8:14])
    pos = 14
    # raw events: (tick, kind, payload)
    raw = []
    tempo_changes = [(0, 500000)]  # (tick, usec per quarter)
    for t in range(ntrk):
        assert data[pos:pos+4] == b'MTrk', (t, data[pos:pos+4])
        tlen = struct.unpack('>I', data[pos+4:pos+8])[0]
        tend = pos + 8 + tlen
        i = pos + 8
        tick = 0
        running = None
        while i < tend:
            dt, i = parse_vlq(data, i)
            tick += dt
            b = data[i]
            if b == 0xFF:
                mtype = data[i+1]
                ln, i2 = parse_vlq(data, i+2)
                payload = data[i2:i2+ln]
                if mtype == 0x51 and ln == 3:
                    usec = (payload[0] << 16) | (payload[1] << 8) | payload[2]
                    tempo_changes.append((tick, usec))
                i = i2 + ln
            elif b in (0xF0, 0xF7):
                ln, i2 = parse_vlq(data, i+1)
                payload = data[i2:i2+ln]
                raw.append((tick, 'sysex', bytes(payload)))
                i = i2 + ln
            else:
                if b & 0x80:
                    status = b; i += 1
                    running = status
                else:
                    status = running
                if status is None:
                    break
                typ = status & 0xF0
                if typ in (0x90, 0x80, 0xA0, 0xB0, 0xE0):
                    d1 = data[i]; d2 = data[i+1]; i += 2
                    raw.append((tick, 'short', (status, d1, d2)))
                elif typ in (0xC0, 0xD0):
                    d1 = data[i]; i += 1
                    raw.append((tick, 'short', (status, d1, 0)))
                else:
                    break
        pos = tend

    # tempo map -> tick to seconds
    tempo_changes = sorted(set(tempo_changes))
    # build segments: list of (tick_start, usec_per_quarter, sec_at_tick_start)
    segs = []
    sec = 0.0
    prev_tick = 0
    prev_usec = tempo_changes[0][1]
    for tick, usec in tempo_changes[1:]:
        sec += (tick - prev_tick) * prev_usec / 1e6 / div
        segs.append((prev_tick, prev_usec, sec - (tick - prev_tick) * prev_usec / 1e6 / div))
        prev_tick, prev_usec = tick, usec
    segs.append((prev_tick, prev_usec, sec))

    def tick_to_sec(tick):
        # find segment
        s = segs[0]
        for cand in segs:
            if cand[0] <= tick:
                s = cand
            else:
                break
        t0, usec, sec0 = s
        return sec0 + (tick - t0) * usec / 1e6 / div

    raw.sort(key=lambda e: e[0])
    out = []
    for tick, kind, payload in raw:
        t = tick_to_sec(tick)
        if t > args.seconds:
            continue
        frame = int(t * args.rate)
        if kind == 'short':
            st, d1, d2 = payload
            out.append('%d SHORT %d %d %d' % (frame, st, d1, d2))
        else:
            out.append('%d SYSEX %s' % (frame, payload.hex()))
    with open(args.outfile, 'w') as f:
        f.write('\n'.join(out) + '\n')
    print('wrote %d events, %d tempo changes, %.1f s' % (len(out), len(tempo_changes), args.seconds))

if __name__ == '__main__':
    main()
