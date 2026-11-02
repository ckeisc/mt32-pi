#!/usr/bin/env python3
"""Compare two raw stereo float32 48 kHz renders: overall and per-second.

Reads the files one second at a time, so renders of any length fit in memory.
"""

import sys

import numpy as np

SR = 48000


def db(x):
    return 20 * np.log10(max(x, 1e-12))


def main(a_path, b_path):
    a = np.memmap(a_path, dtype="<f4", mode="r").reshape(-1, 2)
    b = np.memmap(b_path, dtype="<f4", mode="r").reshape(-1, 2)
    n = min(len(a), len(b))
    if len(a) != len(b):
        print(f"length differs: {len(a)} vs {len(b)} frames")

    same = 0
    first = None
    peak_a = peak_d = 0.0
    sum_a = sum_d = 0.0
    seconds = []
    for start in range(0, n, SR):
        end = min(n, start + SR)
        seg_a = np.asarray(a[start:end], dtype=np.float64)
        seg_d = seg_a - np.asarray(b[start:end], dtype=np.float64)
        differs = np.any(seg_d != 0, axis=1)
        if first is None and differs.any():
            first = start + int(np.argmax(differs))
        same += int(np.count_nonzero(seg_d == 0))
        peak_a = max(peak_a, float(np.abs(seg_a).max()))
        peak_d = max(peak_d, float(np.abs(seg_d).max()))
        ea, ed = float((seg_a**2).sum()), float((seg_d**2).sum())
        sum_a += ea
        sum_d += ed
        if end - start == SR:
            ra, rd = np.sqrt(ea / seg_a.size), np.sqrt(ed / seg_d.size)
            # Seconds without any difference, or without signal, can't be "worst"
            rel = db(rd) - db(ra) if ra > 1e-9 and rd > 1e-12 else -999
            seconds.append((rel, start // SR, db(ra), db(rd)))

    print(f"frames {n} ({n / SR:.1f} s), identical samples {same / (2 * n) * 100:.2f}%")
    if first is not None:
        print(f"first difference at {first / SR:.4f} s")
    rms_a, rms_d = np.sqrt(sum_a / (2 * n)), np.sqrt(sum_d / (2 * n))
    print(f"peak A {db(peak_a):.1f} dBFS, rms A {db(rms_a):.1f} dBFS")
    print(f"max |diff| {peak_d:.3e} ({db(peak_d):.1f} dBFS), ", end="")
    print(f"rms diff {db(rms_d):.1f} dBFS")
    seconds.sort(reverse=True)
    print("worst seconds (diff relative to signal):")
    for rel, s, ra, rd in [x for x in seconds if x[0] > -999][:5]:
        print(
            f"  {s:3d}s  signal {ra:6.1f} dBFS  diff {rd:6.1f} dBFS  rel {rel:6.1f} dB"
        )


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
