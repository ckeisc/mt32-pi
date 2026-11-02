#!/usr/bin/env python3
"""Copy a Standard MIDI File keeping only the given MIDI channels.

Channel messages (notes, controllers, program changes, pitch bend, ...) on
other channels are removed; SysEx and meta events (tempo, etc.) are kept, so
the result plays with the same setup and timing as the original.

Usage: filter_midi.py INPUT.mid OUTPUT.mid CHANNEL [CHANNEL...]
  CHANNEL is a MIDI channel number, 1-16.
"""

import struct
import sys
from pathlib import Path


def read_var_len(data, pos):
    value = 0
    while True:
        byte = data[pos]
        pos += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, pos


def var_len(value):
    out = [value & 0x7F]
    value >>= 7
    while value:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    return bytes(reversed(out))


def channel_data_length(status):
    return 1 if status & 0xF0 in (0xC0, 0xD0) else 2


def filter_track(data, keep):
    """Return the track body with channel messages for other channels removed."""
    out = bytearray()
    pos = 0
    running = None
    pending_delta = 0
    while pos < len(data):
        delta, pos = read_var_len(data, pos)
        pending_delta += delta
        status = data[pos]

        if status == 0xFF:  # meta event
            length, end = read_var_len(data, pos + 2)
            event = data[pos : end + length]
            pos = end + length
        elif status in (0xF0, 0xF7):  # SysEx, or escaped/continued SysEx
            length, end = read_var_len(data, pos + 1)
            event = data[pos : end + length]
            pos = end + length
            running = None
        else:
            if status & 0x80:
                running = status
                pos += 1
            elif running is None:
                raise ValueError(
                    f"data byte without running status at track offset {pos}"
                )
            count = channel_data_length(running)
            event = bytes([running]) + data[pos : pos + count]
            pos += count
            if (running & 0x0F) not in keep:
                continue  # drop it; its delta time carries over to the next event

        out += var_len(pending_delta) + event
        pending_delta = 0
    return bytes(out)


def filter_file(source, keep):
    if source[:4] != b"MThd":
        raise ValueError("not a Standard MIDI File")
    header_length = struct.unpack(">I", source[4:8])[0]
    out = bytearray(source[: 8 + header_length])
    pos = 8 + header_length
    while pos + 8 <= len(source):
        chunk_type = source[pos : pos + 4]
        length = struct.unpack(">I", source[pos + 4 : pos + 8])[0]
        body = source[pos + 8 : pos + 8 + length]
        if chunk_type == b"MTrk":
            body = filter_track(body, keep)
        out += chunk_type + struct.pack(">I", len(body)) + body
        pos += 8 + length
    return bytes(out)


def main():
    if len(sys.argv) < 4:
        print(__doc__.strip().splitlines()[-2], file=sys.stderr)
        return 2
    try:
        keep = {int(arg) - 1 for arg in sys.argv[3:]}
    except ValueError:
        print("Channels must be numbers 1-16", file=sys.stderr)
        return 2
    if not all(0 <= ch <= 15 for ch in keep):
        print("Channels must be numbers 1-16", file=sys.stderr)
        return 2
    source = Path(sys.argv[1]).read_bytes()
    Path(sys.argv[2]).write_bytes(filter_file(source, keep))
    return 0


if __name__ == "__main__":
    sys.exit(main())
