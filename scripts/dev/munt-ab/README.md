# Munt A/B testing

Tools for checking what a Munt (libmt32emu) upgrade changes in MT-32 output. There are two complementary tests:

1. **Offline renders (exact):** render MIDI files through the old and new library on a PC and compare them sample by sample. Shows precisely which files change and where.
2. **Hardware recordings (end to end):** record the same MIDI on two SD cards, old build and new build, and listen. Confirms the change behaves on a real Pi, through mt32-pi's resampler and audio output.

Run the offline test first: it tells you which passages to listen to closely on hardware.

## What to expect for 2.7.0 → 2.8.3

Most music should render **identically**. Differences are expected only where these library changes apply:

| Change | Version | When you'll hear it |
| --- | --- | --- |
| More accurate partial allocator for old-gen units | 2.8.0 (#109) | Dense passages that run out of the 32 partials, with an MT-32 v1.x control ROM |
| Timbre change by SysEx makes held notes decay | 2.7.3 (#132) | Games that rewrite a part's timbre while notes sound |
| Invalid short MIDI messages ignored | 2.8.0, 2.8.3 (#134, #147) | Only with malformed MIDI input |
| Performance improvements | 2.8.0 (#131) | No audible change; lower CPU use |

Expected results for the generated test set:

| File | MT-32 v1.x ROM (old-gen) | MT-32 v2.x, CM-32L (new-gen) |
| --- | --- | --- |
| `control_ensemble` | identical | identical |
| `partial_stealing` | differs from 2.0 s to 3.5 s, and again at 6.0 s and 10.0 s | identical |
| `timbre_sysex_held` | differs from 1.0 s | differs from 1.0 s |

`partial_stealing` uses a one-partial sine timbre uploaded by SysEx, so its partial counts don't depend on the ROM's own sounds. A difference anywhere else, such as in `control_ensemble`, would point to a problem.

## 1. Offline renders

Needs a Linux machine or WSL2 with `git`, `cmake`, a C++ compiler, `pkg-config`, `libglib2.0-dev` and `python3`, plus your MT-32 and/or CM-32L ROM files. Nothing is installed system-wide.

```sh
scripts/dev/munt-ab/run.sh --roms /path/to/roms --machine mt32
```

The script clones Munt into `./munt-ab-work`, builds the `mt32emu-smf2wav` renderer from one source tree against each library version, generates a small test set (`make_test_midi.py`), renders it, and prints a comparison. The output looks like this (numbers illustrative):

```
control_ensemble.wav   identical
partial_stealing.wav   DIFFERS from 2.002 s; max sample diff 32483; difference level -13.4 dBFS
timbre_sysex_held.wav  DIFFERS from 1.000 s; ...
```

Useful options:

- `--machine mt32_1_07` (or whichever v1.x set you own): only old-gen ROMs show the partial allocator change. `--machine mt32` picks your *highest* MT-32 ROM version, which is new-gen if you have a v2.x set. Run again with `--machine cm32l` for the CM-32L.
- `--midi DIR`: render your own MIDI files, e.g. favourite game soundtracks, instead of the generated set.
- `--sysex FILE`: send a setup file before each MIDI file, as you do with the device (raw `.syx`, or a `.mid` of SysEx). Most Sierra soundtracks need their instrument upload to sound right. Repeatable; files are sent in order.
- `--solo N`: render only MIDI channel N (SysEx is kept, so the setup still applies) and print the loudness of every second, old vs. new. Seconds that are audible in the old render but at least 20 dB quieter in the new one are marked `much quieter in new`. Use it to check whether one instrument goes missing, without listening.
- `--old` / `--new`: compare other Munt git refs.

Renders are written to `munt-ab-work/render-<machine>-old/` and `-new/` for listening in any audio editor. Settings mirror mt32-pi: COARSE analog filter, 48 kHz, GOOD resampler quality. Leading silence is kept, so reported times match positions in the MIDI file. For the generated set, `timbre_sysex_held` sends its first timbre change at 1.0 s.

Re-running is quick: the Munt clone and renderer builds are reused.

### Partial report

When notes go missing, `--report` shows what the new library did with every note, using the same MIDI timing as mt32-pi:

```sh
scripts/dev/munt-ab/run.sh --roms /path/to/roms --machine mt32_1_07 --report \
    --sysex /path/to/setup.syx --channel 6 --midi /path/to/midi-files
```

It prints:

- **Every Note On the allocator ignored** for lack of partials, and **every one that made it cut short** a playing note. Each line has the time, MIDI channel, key, and how many notes each part was playing.
- **Each part's setup** at the end: MIDI channel, instrument (group and number from Patch Temp, name from its working copy), output level and pan. A blank name usually means an empty instrument.
- A **per-channel summary** of ignored and cut-short notes, and a note if the setup file writes past the 64 user instrument slots, which Munt 2.8.2 and later ignore.
- With `--channel N`, a **timeline** in half-second windows: notes received, the most notes playing at once on that channel's part, and the part's volume. Windows where notes arrive but none play are marked `no notes playing`, and windows where notes play at volume 0 are marked `volume 0`. Program changes on the part are listed above the timeline.

`--report` needs a specific machine ID (`mt32_1_07`, `mt32_2_04`, `cm32l_1_02` …), not `mt32` or `cm32l`.

## 2. Hardware recordings

Use two SD cards that differ only in the kernel:

- **A (old):** the current release, or a stock mt32-pi card.
- **B (new):** the same card contents with the test build's kernel copied over (from the pull request's CI `sdcard` artifact).

Keep everything else identical: `mt32-pi.cfg`, ROMs, SoundFonts, output device and volume.

For each card:

1. Boot, and confirm MT-32 mode with the ROM set under test.
2. Record the Pi's output at 48 kHz in Audacity or similar, with the same input gain for both cards.
3. Play the same files from the same MIDI source: the generated set (`make_test_midi.py OUTPUT_DIR`) plus two or three game tracks you know well, ideally ones that upload custom timbres by SysEx (most Sierra and LucasArts titles).
4. Also check the LCD (SysEx text, part levels), ROM set switching, and that the log shows no `Sound data dropped`.

Recordings through a DAC and audio interface won't null perfectly, so compare by ear, and by lining up the waveforms where the offline test reported a difference.
