#!/usr/bin/env python3
"""Generate MIDI files that exercise MT-32 behaviour changed between Munt releases.

Writes Standard MIDI Files (format 0) into the given directory:

  control_ensemble.mid       Plain multi-part playback. Expected to render
                             identically across library versions.
  partial_stealing.mid       Runs out of partials while another part has a
                             releasing note. Old-gen units (MT-32 v1.x ROMs)
                             steal from the part's own held notes; new-gen units
                             cut the other part's releasing note. Munt 2.8.0
                             emulates the old-gen behaviour (#109). Uses a
                             one-partial sine timbre uploaded by SysEx, so it
                             behaves the same with any ROM set.
  timbre_sysex_held.mid      Changes a part's timbre via SysEx while notes are
                             held. Since 2.7.3 the held notes decay at once,
                             like the real hardware (#132).

Usage: make_test_midi.py OUTPUT_DIR
"""

import struct
import sys
from pathlib import Path

TICKS_PER_BEAT = 480
MT32_PART_CHANNELS = range(1, 9)  # MT-32 default: parts 1-8 on MIDI channels 2-9
RHYTHM_CHANNEL = 9  # MIDI channel 10


def var_len(value):
    """Encode a MIDI variable-length quantity."""
    out = [value & 0x7F]
    value >>= 7
    while value:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    return bytes(reversed(out))


def mt32_sysex(address, data, device=0x10):
    """Build a Roland DT1 SysEx message for the MT-32.

    Device ID 0x10 uses absolute addresses. Device IDs 0x00-0x0F address the
    part on that MIDI channel, so 0x000000 is its Patch Temp area and 0x020000
    its Timbre Temp area.
    """
    body = list(address) + list(data)
    checksum = (128 - sum(body) % 128) % 128
    return bytes([0xF0, 0x41, device, 0x16, 0x12] + body + [checksum, 0xF7])


# One-partial sine wave timbre, based on the one in Munt's own unit tests
# (mt32emu/src/test/TestUtils.cpp, sendSineWaveSysex), plus a TVA release time
# so released notes keep sounding briefly, as most real timbres do.
SINE_RELEASE_TIME = 30
SINE_PATCH = (0x00, 0x00, 0x18, 0x19, 0x0C, 0x00, 0x00, 0x00, 0x64, 0x00)
SINE_TIMBRE = (
    b"Test-sine."
    + bytes([0x00, 0x00, 0x01, 0x00])
    + bytes([0x18, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x07])
    + bytes([0x00] * 15 + [0x32])
    + bytes([0x00, 0x0B, 0x00, 0x07] + [0x00] * 12)
    + bytes([0x00, 0x64, 0x32, 0x00, 0x0C, 0x00, 0x0C, 0x00])
    + bytes([0x00] * 5 + [SINE_RELEASE_TIME] + [0x64] * 4)  # TVA env times, levels
)


def upload_sine_timbre(track, tick, channel):
    track.sysex(tick, mt32_sysex((0x00, 0x00, 0x00), SINE_PATCH, device=channel))
    track.sysex(tick, mt32_sysex((0x02, 0x00, 0x00), SINE_TIMBRE, device=channel))


class Track:
    """Collects absolute-time events and serialises them as an SMF track."""

    def __init__(self):
        self.events = []

    def msg(self, tick, *data):
        self.events.append((tick, len(self.events), bytes(data)))

    def sysex(self, tick, message):
        # SMF stores F0 sysex as: F0 <length> <bytes after F0>
        payload = message[1:]
        self.events.append(
            (tick, len(self.events), b"\xf0" + var_len(len(payload)) + payload)
        )

    def note(self, tick, length, channel, key, velocity=100):
        self.msg(tick, 0x90 | channel, key, velocity)
        self.msg(tick + length, 0x80 | channel, key, 0)

    def program(self, tick, channel, program):
        self.msg(tick, 0xC0 | channel, program)

    def encode(self):
        out = bytearray()
        last = 0
        # Tempo: 120 BPM
        out += var_len(0) + b"\xff\x51\x03" + (500000).to_bytes(3, "big")
        for tick, _, data in sorted(self.events):
            out += var_len(tick - last) + data
            last = tick
        out += var_len(TICKS_PER_BEAT) + b"\xff\x2f\x00"
        return b"MTrk" + struct.pack(">I", len(out)) + bytes(out)


def write_smf(path, track):
    header = b"MThd" + struct.pack(">IHHH", 6, 0, 1, TICKS_PER_BEAT)
    path.write_bytes(header + track.encode())


def beats(n):
    return int(n * TICKS_PER_BEAT)


def control_ensemble():
    t = Track()
    # Piano, strings, bass, brass-ish on four parts, plus rhythm
    for channel, program in zip(MT32_PART_CHANNELS, (0, 48, 32, 56)):
        t.program(0, channel, program)
    melody = [60, 62, 64, 65, 67, 69, 71, 72]
    for i, key in enumerate(melody * 2):
        start = beats(1 + i * 0.5)
        t.note(start, beats(0.45), 1, key)
        if i % 2 == 0:
            t.note(start, beats(0.9), 2, key - 12, 80)
            t.note(start, beats(0.9), 3, 36 + (i % 4) * 2, 90)
        if i % 4 == 0:
            t.note(start, beats(1.8), 4, key + 7, 70)
        t.note(start, beats(0.2), RHYTHM_CHANNEL, 36 if i % 2 == 0 else 38, 110)
        t.note(start, beats(0.1), RHYTHM_CHANNEL, 42, 80)
    return t


def partial_stealing():
    t = Track()
    part1, part2 = 1, 2  # MIDI channels 2 and 3
    # Partial reserve: part 1 = 31, parts 2-8 = 0, rhythm = 1
    t.sysex(0, mt32_sysex((0x10, 0x00, 0x04), (31, 0, 0, 0, 0, 0, 0, 0, 1)))
    upload_sine_timbre(t, 0, part1)
    upload_sine_timbre(t, 0, part2)

    for rep in range(3):
        start = beats(2 + rep * 8)
        # Part 1 holds 31 one-partial notes: exactly its reserve
        for i in range(31):
            t.note(start + i * 8, beats(5), part1, 36 + i, 40)
        # Part 2 plays one note and lets go: a releasing note over its reserve.
        # Part 1 asks for one more partial on the same tick, while all 32 are
        # still busy (this timbre's release is very short).
        t.note(start + beats(1), beats(1), part2, 96, 110)
        t.note(start + beats(2), beats(2), part1, 84, 110)
    return t


def timbre_sysex_held():
    t = Track()
    channel = 1  # MT-32 part 1
    t.program(0, channel, 48)
    for i, start in enumerate((beats(1), beats(5), beats(9))):
        # Hold a chord, then change part 1's timbre number mid-note.
        for key in (60, 64, 67):
            t.note(start, beats(3), channel, key)
        # Patch Temp area for part 1 starts at 03 00 00; byte 1 is timbre number
        t.sysex(start + beats(1), mt32_sysex((0x03, 0x00, 0x01), (10 + i * 5,)))
    return t


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    out_dir = Path(sys.argv[1])
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, build in (
        ("control_ensemble.mid", control_ensemble),
        ("partial_stealing.mid", partial_stealing),
        ("timbre_sysex_held.mid", timbre_sysex_held),
    ):
        write_smf(out_dir / name, build())
        print(out_dir / name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
