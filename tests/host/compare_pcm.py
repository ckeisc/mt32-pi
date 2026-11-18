#!/usr/bin/env python3
"""Compare two PCM WAV files without resampling or time alignment."""
import argparse
import math
import struct
import sys
import wave


def read_pcm(path):
    with wave.open(path, 'rb') as w:
        params = (w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getcomptype())
        frames = w.readframes(w.getnframes())
    channels, width, rate, compression = params
    if channels != 2 or width != 2 or compression != 'NONE':
        raise ValueError(f'{path}: expected stereo 16-bit uncompressed PCM, got '
                         f'{channels}ch/{width * 8}bit/{compression}')
    count = len(frames) // 2
    return rate, struct.unpack('<' + 'h' * count, frames)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('reference', help='MAME-rendered WAV')
    ap.add_argument('candidate', help='libmu50-rendered WAV')
    ap.add_argument('--max-diff', type=int, default=0,
                    help='pass if every sample differs by at most this amount (default: 0)')
    args = ap.parse_args()
    try:
        rr, a = read_pcm(args.reference)
        cr, b = read_pcm(args.candidate)
    except (OSError, wave.Error, ValueError) as e:
        print(f'ERROR: {e}', file=sys.stderr)
        return 2
    if rr != cr:
        print(f'FAIL: sample-rate mismatch: reference={rr}, candidate={cr}')
        return 1
    if len(a) != len(b):
        print(f'FAIL: sample-count mismatch: reference={len(a)}, candidate={len(b)} '
              f'(stereo frames {len(a)//2} vs {len(b)//2})')
        return 1
    diffs = [abs(x-y) for x, y in zip(a, b)]
    mismatches = sum(d != 0 for d in diffs)
    maximum = max(diffs, default=0)
    rms = math.sqrt(sum(d*d for d in diffs) / len(diffs)) if diffs else 0.0
    first = next((i for i, d in enumerate(diffs) if d), None)
    print(f'rate: {rr} Hz; frames: {len(a)//2}; samples: {len(a)}')
    print(f'exact matches: {len(a)-mismatches}/{len(a)} ({100.0*(len(a)-mismatches)/max(1,len(a)):.6f}%)')
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
