# MU50 SysEx in non-split mode — problem for review

Branch: `wip/mu50-sysex-nonsplit` (ckeisc/mt32-pi), commit 71772a73.
This file is a review note, not a design doc. Delete it before any merge.

## What this branch contains

1. **SWP00 overhaul to MAME HEAD (8be57cd).** `external/mu50/src/swp00_0277.cpp/.hpp`
   and `external/mu50/src/synth.cpp` rewritten to the block-struct model, with the
   output-staging gain fix (`routed >> 2` in `Synth::render`), `state_r()` reg 0x01
   readback, and the MEG input scale. BELMO v2 is bit-identical to the 0.277 port.
2. **SysEx-burst fix.** The test song (MU50-The-Musithm.mid) sends 50 SysEx /
   1523 bytes in the first 5.78 s before the first note. The 256-byte H8 MIDI queue
   and the 1-byte SCI RDR dropped them, so the XG setup (incl. variation type 0x4b
   distortion) never arrived and the synth played unconfigured and quiet
   (`native_peak` 8337 vs MAME 14845). Fix in `src/synth/mu50h8.h/.cpp`:
   MIDI queue 256 -> 4096 bytes, plus a 256-byte RX FIFO behind the SCI RDR.
   In **split mode** the diag confirms the peak is fixed (`native_peak` 15485–16671).
3. **FIR resampler option** (`[mu50] resampler = kaiser`, default cubic).
   64-tap polyphase windowed-sinc, Kaiser beta=10. Kept as an option; user
   reverted to cubic.

## The open problem: SysEx delivery in NON-split mode

MU50 runs in split mode on the Pi by default (H8 on one core, SWP render on
another). There is a test kernel with split forced off (`SetSplitMode(false)`)
to isolate a suspected volume loss. The user listened and reports the non-split
kernel **sounds about the same volume as split** — it is NOT louder and NOT
quieter. (An earlier `native_peak` comparison suggested non-split was half the
level; that metric is a single-sample max per 5 s window and is contradicted by
listening, so do not trust it as a loudness measure.)

The real question is whether SysEx is **reliably delivered** in non-split mode.
Reading the code, there is a thread-safety difference between the two paths:

- **Split mode** (`src/synth/mu50synth.cpp`): the MIDI thread only pushes to
  `m_MidiStage` (a lock-free SPSC queue). The audio thread drains it in
  `DrainMidiStage()` (called from `AdvanceH8`) and is the only caller of
  `Mu50H8::queue_midi_byte()`. `Mu50H8::midi_line_tick()` (which advances the
  31250-baud line driver and pops bytes from the same queue) also runs on the
  audio thread, inside the H8 stepping. So `queue_midi_byte` and
  `midi_line_tick` are single-threaded in split mode.
- **Non-split mode**: `HandleMIDISysExMessage()` and `HandleMIDIShortMessage()`
  (via `QueueMIDIMessageBytes()`) call `Mu50H8::queue_midi_byte()` **directly
  from the MIDI thread**, guarded only by `m_Lock`. But `midi_line_tick()` still
  runs on the audio thread during H8 stepping, and it reads/writes the same
  shared state (`midi_queue_`, `midi_q_head_`, `midi_q_tail_`, `midi_byte_`,
  `midi_bit_`, `midi_edge_`) **without holding `m_Lock`**. So during a dense
  burst the MIDI thread and the audio thread can interleave inside
  `queue_midi_byte` / `midi_line_tick` and corrupt the queue indices, dropping
  or duplicating bytes.

Evidence that something is off in non-split: in the non-split diag
(`mt32pi-diag-40.log`, MU50 segment) the first window after the switch shows
`midi_queued=2271` vs `sci_read=699` — the firmware is 1572 bytes behind,
which is almost exactly the 1523-byte SysEx burst. It catches up in later
windows (`sci_read == midi_queued`), so the bytes may only be delayed, not
lost — but the lag shows the two paths do not behave the same, and a race
during the burst is the most plausible cause.

## Questions for the reviewer

1. Is the race described above real? Walk through `queue_midi_byte()` in
   `src/synth/mu50h8.cpp` and `midi_line_tick()` and confirm whether the MIDI
   thread (non-split `HandleMIDISysExMessage` / `QueueMIDIMessageBytes`) can
   interleave with the audio thread's `midi_line_tick` in a way that loses
   bytes. If it is not a race, say why (e.g. is `midi_line_tick` actually
   serialized against the MIDI thread somewhere I missed?).
2. What is the minimal correct fix? The obvious one is to make non-split use
   the same staging as split: always push MIDI into `m_MidiStage` from the
   handlers, and always call `DrainMidiStage()` from `AdvanceH8` regardless of
   `m_bSplitMode`. Is that safe in single-core mode (ordering, latency, the
   `m_Lock` in the non-split path, note tracking in `DrainMidiStage` vs the
   handler)? Is there a smaller fix (e.g. take `m_Lock` in `midi_line_tick`,
   or make the queue indices atomic)?
3. Separate volume question: with the peak fixed in split mode, the Pi MU50
   RMS on the Musithm is still ~1.8x below the host/MAME level (Pi recording
   Nuked/MU50 ratio 2.43x vs host 1.30–1.34x), while the host replay is
   MAME-accurate (corr 0.952). Split mode is exonerated as a cause (code read
   finds no factor of 2; the non-split test kernel sounds the same). The
   resampler is ruled out (cubic and FIR give the same ratio) and mono
   phase-cancellation is ruled out (host MU50 mono/stereo 0.935, Nuked
   0.909–0.938). Where else should I look for a ~1.8x average-level loss on
   the Pi that does not show up in the host replay or in `native_peak`?

## Ground rules for the reviewer

- Do not invent a fix without reading the code. Cite file and line.
- `native_peak` is a per-window single-sample max of the pre-resample native
  frames; it is not an RMS and not a loudness measure.
- The user's listening is ground truth for volume. Do not override it with a
  metric.
- Do not commit or push anything; this is a review only.
