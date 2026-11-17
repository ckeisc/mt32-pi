#!/usr/bin/env python3
"""Compare two raw s16 stereo interleaved PCM files (xg_render output).

Prints the same stats as compare_pcm.py (which needs WAV): exact
matches, mismatched samples, max abs diff, RMS diff, first mismatch.
Exit 0 when every sample differs by at most --max-diff (default 0,
i.e. bit-identical), else 1. Sample-count mismatch is a failure.
"""
import argparse
import math
import struct
import sys


def read_raw(path):
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) % 2 != 0:
        raise ValueError(f'{path}: odd byte count {len(data)}')
    return struct.unpack('<' + 'h' * (len(data) // 2), data)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('reference')
    ap.add_argument('candidate')
    ap.add_argument('--max-diff', type=int, default=0)
    args = ap.parse_args()
    try:
        a = read_raw(args.reference)
        b = read_raw(args.candidate)
    except (OSError, ValueError) as e:
        print(f'ERROR: {e}', file=sys.stderr)
        return 2
    if len(a) != len(b):
        print(f'FAIL: sample-count mismatch: reference={len(a)}, candidate={len(b)} '
              f'(stereo frames {len(a)//2} vs {len(b)//2})')
        return 1
    diffs = [abs(x - y) for x, y in zip(a, b)]
    mismatches = sum(d != 0 for d in diffs)
    maximum = max(diffs, default=0)
    rms = math.sqrt(sum(d * d for d in diffs) / len(diffs)) if diffs else 0.0
    first = next((i for i, d in enumerate(diffs) if d), None)
    print(f'frames: {len(a)//2}; samples: {len(a)}')
    print(f'exact matches: {len(a)-mismatches}/{len(a)} ({100.0*(len(a)-mismatches)/max(1, len(a)):.6f}%)')
    print(f'mismatched samples: {mismatches}; max abs diff: {maximum}; RMS diff: {rms:.6f}')
    if first is not None:
        print(f'first mismatch: interleaved sample {first} '
              f'(frame {first//2}, {"L" if first % 2 == 0 else "R"}); '
              f'reference={a[first]}, candidate={b[first]}, diff={b[first]-a[first]}')
    ok = maximum <= args.max_diff
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
