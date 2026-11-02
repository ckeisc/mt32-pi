#!/usr/bin/env python3
"""Write a deterministic General MIDI stress file for FluidSynth A/B renders.

Covers all 128 programs, GM drums, chords up to ~100 voices, pitch bend sweeps,
modulation, expression, sustain, reverb/chorus sends and a GS reset.
"""

import struct
import sys

TPQ = 480


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.insert(0, (n & 0x7F) | 0x80)
        n >>= 7
    return bytes(out)


def track(events):
    events.sort(key=lambda e: (e[0], e[1]))
    data, last = b"", 0
    for tick, _order, msg in events:
        data += vlq(tick - last) + msg
        last = tick
    data += vlq(0) + b"\xff\x2f\x00"
    return b"MTrk" + struct.pack(">I", len(data)) + data


def main(path):
    ev, order = [], 0

    def add(tick, msg):
        nonlocal order
        ev.append((tick, order, bytes(msg)))
        order += 1

    # tempo 120 bpm, GS reset
    add(0, b"\xff\x51\x03" + struct.pack(">I", 500000)[1:])
    gs = [0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7]
    add(0, bytes([0xF0, len(gs)]) + bytes(gs))

    beat = TPQ
    t = beat
    # Section 1: every program, 15 melodic channels, short phrases
    for prog in range(128):
        ch = [0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15][prog % 15]
        add(t, [0xB0 | ch, 91, (prog * 7) % 128])
        add(t, [0xB0 | ch, 93, (prog * 13) % 128])
        add(t, [0xB0 | ch, 10, (prog * 29) % 128])
        add(t, [0xC0 | ch, prog])
        for i, key in enumerate((48, 55, 60, 64, 67)):
            add(t + i * 20, [0x90 | ch, key + prog % 12, 60 + (i * 13) % 60])
            add(t + beat // 2 + i * 20, [0x80 | ch, key + prog % 12, 0])
        if prog % 4 == 0:
            for s in range(16):
                v = 8192 + int(8191 * ((s - 8) / 8))
                add(t + s * 15, [0xE0 | ch, v & 0x7F, (v >> 7) & 0x7F])
            add(t + beat // 2, [0xE0 | ch, 0x00, 0x40])
        # drums under it
        add(t, [0x99, 35 + prog % 47, 100])
        add(t + beat // 4, [0x99, 42, 80])
        add(t + beat // 2, [0x89, 35 + prog % 47, 0])
        t += beat // 2

    # Section 2: dense sustained chords to push polyphony
    for ch, prog in ((0, 48), (1, 0), (2, 88), (3, 19), (4, 52), (5, 61)):
        add(t, [0xC0 | ch, prog])
        add(t, [0xB0 | ch, 64, 127])
        add(t, [0xB0 | ch, 1, 0])
    for bar in range(8):
        for ch in range(6):
            for k in range(8):
                key = 36 + ch * 6 + k * 5 + bar % 3
                add(t + k * 10, [0x90 | ch, key, 50 + 8 * k])
                add(t + 4 * beat - 1, [0x80 | ch, key, 0])
        for s in range(32):
            add(t + s * (4 * beat // 32), [0xB0 | (bar % 6), 1, (s * 4) % 128])
            add(t + s * (4 * beat // 32), [0xB0 | (bar % 6), 11, 127 - (s * 3) % 64])
        add(t, [0x99, 49, 110])
        add(t + beat, [0x99, 38, 100])
        add(t + 2 * beat, [0x99, 36, 120])
        add(t + 3 * beat, [0x99, 38, 100])
        t += 4 * beat
    for ch in range(6):
        add(t, [0xB0 | ch, 64, 0])
        add(t, [0xB0 | ch, 123, 0])

    data = b"MThd" + struct.pack(">IHHH", 6, 1, 1, TPQ) + track(ev)
    with open(path, "wb") as f:
        f.write(data)


if __name__ == "__main__":
    main(sys.argv[1])
