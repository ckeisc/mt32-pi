//
// mu50synth.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
// Copyright (C) 2020-2023 Dale Whinham <daleyo@gmail.com>
//
// This file is part of mt32-pi.
//
// mt32-pi is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// mt32-pi is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// mt32-pi. If not, see <http://www.gnu.org/licenses/>.
//

#ifndef _mu50synth_h
#define _mu50synth_h

#include <circle/types.h>

#include <vector>

#include "synth/synthbase.h"
#include "utility.h"
#include "ringbuffer.h"
#include "synth/mu50spsc.h"

// The libmu50 class is only forward-declared here, rather than including
// the libmu50 header: libmu50 requires C++17, while the mt32-pi kernel is
// built as C++14. Only mu50synth.cpp includes the real header, and it is
// compiled as C++17 (see Kernel.mk). This mirrors the EmuSC port's pattern.
namespace libmu50
{
	class Synth;
}

// Same pattern for the H8 CPU port: only mu50synth.cpp includes the
// real header (also C++17, see Kernel.mk).
namespace mu50h8
{
	class Mu50H8;
}

class CMU50Synth : public CSynthBase
{
public:
	CMU50Synth(unsigned nSampleRate, float nGain);
	virtual ~CMU50Synth() override;

	// CSynthBase
	virtual bool Initialize() override;
	virtual void HandleMIDIShortMessage(u32 nMessage) override;
	virtual void HandleMIDISysExMessage(const u8* pData, size_t nSize) override;
	virtual bool IsActive() override;
	virtual void AllSoundOff() override;
	virtual void SetMasterVolume(u8 nVolume) override;
	virtual size_t Render(s16* pOutBuffer, size_t nFrames) override;
	virtual size_t Render(float* pOutBuffer, size_t nFrames) override;
	virtual void ReportStatus() const override;
	virtual void UpdateLCD(CLCD& LCD, unsigned int nTicks) override;

	// H8 hook targets, called from the file-local hook functions in
	// mu50synth.cpp. Public only because the file-local hooks cannot
	// call private methods. M1: writes push cycle-stamped events
	// into the W-event FIFO (drained before each block render);
	// reads drain first, then read live register state, so no read
	// ever observes a stale register. M2 split mode: writes push
	// (waiting on full, never dropping) and update the audio-side
	// shadow; reads use pending-scan newest-wins with the shadow as
	// fallback, and never touch live SWP state (SWP-thread-owned).
	void OnH8SwpWrite(u64 nCycle, u16 nAddr, u8 nData);
	u8 OnH8SwpRead(u16 nAddr);
	// SCI1 RDR read by the firmware (see OnH8Sci1RdrRead).
	void OnH8Sci1RdrRead();

	// M2 split-mode API (host harness + M4 PCMTask dispatch). The
	// worker thread calls PCMTaskIteration() unconditionally in a
	// loop; gating is inside -- never gate the call on the active
	// flag, or the park ack deadlocks (Nuked wedge rule).
	// SetSplitMode must be called before the SWP thread starts.
	void SetSplitMode(bool bSplit);
	void PCMTaskIteration();
	bool ParkSwp(); // request park, wait for ack; false on timeout
	void UnparkSwp();
	// Test-only atomic snapshots (safe from any thread).
	u64 GetWqFullWaits();
	u64 GetRingDrops();
	u64 GetBlockWaits();
	u64 GetStageDrops();
	u64 GetRenderedEnqueued();
	u64 GetConsumed();
	u64 GetUnderrunFrames(); // cumulative silence-inserted pulls
	// M4 switch park/reactivate (mirror Nuked's SplitActivate /
	// SplitDeactivate, called from CMT32Pi::SwitchSynth). No-ops
	// when split mode is off. Deactivate parks the SWP worker and
	// waits for audio-idle; on timeout it unparks and proceeds
	// (worker stays live but idle-draining -- safe, since the
	// switch already moved m_pCurrentSynth).
	void SplitActivate();
	void SplitDeactivate();
	bool SplitQueuesQuiescent();
	void SetWEventFifoCapForTest(size_t nCap);

private:
	struct TROMPaths
	{
		CString WaveROMPaths[2];
		size_t nWaveROMCount = 0;
		CString FirmwarePaths[8];
		size_t nFirmwareCount = 0;
		bool bValid           = false;
	};

	bool ScanROMs(TROMPaths& OutPaths);
	bool ReadFile(const char* pPath, u8* pBuffer, size_t nSize);

	// Renders libmu50 native-rate (44.1 kHz) frames into pOutBuffer,
	// resampling to the engine's output rate. Single-core mode:
	// caller must hold m_Lock (advances the resampler position and
	// uses the scratch buffers). Split mode: m_Lock is NOT held
	// (audio thread owns H8 + resampler; SWP thread owns registers).
	void RenderResampled(float* pOutBuffer, size_t nFrames);

	// Split-mode RenderResampled (lag-2 pipeline, see the .cpp).
	void RenderResampledSplit(float* pOutBuffer, size_t nFrames);

	// Resample one pulled native block: activity + peak scans, cubic
	// interpolation, prev/leftover carry update. Extracted verbatim
	// from M1's RenderResampled body. m_NativeScratch must hold
	// nNativeRender frames on entry. Returns the new fractional
	// carry. Audio-thread only.
	double ResamplePulledBlock(float* pOutBuffer, size_t nOutDone, size_t nBlockOut, double dBlockStart, size_t nNativeRender);

	// Wait for the oldest pending block's frames, pull them into
	// m_NativeScratch, and resample them. Audio-thread only.
	void ResamplePendingFront(float* pOutBuffer);

	// Advance the H8 by the emulated cycles for nNativeFrames of
	// audio (10 MHz / 44.1 kHz). Single-core mode: caller must hold
	// m_Lock. Split mode: no m_Lock; drains the MIDI staging queue
	// first, then steps and publishes the block.
	void AdvanceH8(size_t nNativeFrames);

	// Apply pending W-event FIFO records up to a producer in-index
	// snapshot (block publish bound). Single-core DrainWEvents()
	// drains everything (snapshot = current in-index). SWP-thread
	// only in split mode; single-core only in single mode -- never
	// concurrent, since modes are exclusive.
	void DrainWEventsUpTo(size_t nSnap);
	void DrainWEvents();

	// Drain the MIDI staging queue into SCI1 (split mode, audio
	// thread only, called at each AdvanceH8).
	void DrainMidiStage();

	// Queue one short message's bytes into SCI1 RX (Nuked byte-length
	// rules: 2 bytes for 0xC0/0xD0 channel messages, 3 otherwise;
	// 2 for 0xF1/0xF3, 3 for 0xF2, 1 for other system messages).
	// Caller must hold m_Lock.
	void QueueMIDIMessageBytes(u8 nStatus, u8 nData1, u8 nData2);

	libmu50::Synth* m_pSynth;

	// The H8 CPU port running the real MU50 firmware. It holds a
	// pointer to m_FirmwareROM, which is therefore kept alive for
	// the H8's lifetime and never modified after creation.
	mu50h8::Mu50H8* m_pH8;
	std::vector<u8> m_FirmwareROM;

	// H8 advance state (caller must hold m_Lock)
	double m_dH8CycleCarry; // fractional emulated cycles into the next block
	bool m_bH8Halted;       // sticky: H8 stopped on an illegal opcode

	// Activity tracking for IsActive(), mirroring the Nuked pattern:
	// audible output within the last 0.5 s of native frames, or a
	// held/sustained note, keeps the clock up.
	u64 m_nWrittenNativeFrames;
	u64 m_nLastActiveNativeFrame;
	unsigned m_nHeldNotes;
	bool m_bSustain;
	bool m_bSustainedNotes;

	// Diag window stats: accumulated under m_Lock in Render, flushed
	// in UpdateLCD outside the lock (mirrors the Nuked window line).
	u64 m_nDiagWindowStartUs;
	u64 m_nDiagCalls;
	u64 m_nDiagTotalUs;
	u64 m_nDiagMaxUs;
	u64 m_nDiagH8Us;
	u64 m_nDiagSwpUs;
	u64 m_nDiagH8Insns;
	u64 m_nDiagOutputFrames;
	// Per-window MIDI/SWP diagnostic counters, flushed in UpdateLCD
	// alongside the timing stats (see the second diag line): bytes
	// accepted into the SCI1 queue, SCI1 RDR reads by the firmware,
	// SWP00 register writes, and the peak native SWP sample before
	// resampling (proves non-zero SWP output independent of gain).
	u64 m_nDiagMidiQueued;
	u64 m_nDiagSciRead;
	u64 m_nDiagSwpWrites;
	int m_nDiagNativePeak;
	// M3: audio-thread wait spin time inside ResamplePendingFront
	// (split mode), windowed like the other diag stats. The window
	// line's audio load is total Render time minus this wait.
	u64 m_nDiagWaitUs;
	// M3: last reported cumulative underrun count, for the window
	// line's per-window underrun delta (the atomic itself is never
	// reset, so host getters stay cumulative).
	u64 m_nDiagLastUnderrunFrames;
	u64 m_nDiagLastSwpRenderUs;

	float m_nGain;
	u8 m_nVolume;

	// Cubic Catmull-Rom resampler state (same 4-tap weights as
	// Nuked's cubic resampler; implemented locally so Nuked's files
	// are untouched).
	double m_dResampleRatio; // native frames per output frame
	double m_dResampleCarry; // fractional native position into the next block
	bool m_bResamplePrimed;  // false until the first block has rendered
	// Three native stereo frames carried across blocks: the frame
	// before the boundary (cubic's prev tap) plus the pair
	// straddling it. Without the prev frame, the first output of
	// each block would lose its fourth tap.
	static constexpr size_t kNativeScratchFrames = 1024;
	s16 m_PrevSample[2];
	s16 m_LeftoverSample[2][2];
	s16 m_NativeScratch[2 * kNativeScratchFrames];
	float m_FloatScratch[1024];

	// M1: W-event FIFO + native sample ring (single-core refactor;
	// no cores yet). The audio core pushes cycle-stamped W records
	// in OnH8SwpWrite and drains them itself before each block
	// render; RenderResampled pulls native frames from the ring
	// instead of using render() output directly. FIFO indices
	// mirror CRingBuffer's convention (full = (in+1)&mask == out,
	// usable 4095); M0 measured worst block burst 602 and worst
	// render-chunk burst 2440, both inside capacity. Ring element
	// mirrors Nuked's TSplitSample shape; M1 fill never exceeds
	// one block (<= 1024 frames) against 16384 capacity.
	// M2: the same FIFO becomes the SPSC cross-core queue (audio
	// pushes, SWP drains; __atomic indices, 64-byte line split in
	// mu50spsc.h). m_nWEventFifoCap is the usable-entry cap for
	// the full check (default 4095 = the whole ring; the tiny-FIFO
	// stress test lowers it -- indices still wrap through all 4096
	// slots, so wrap semantics are exercised even at a small cap).
	struct TWEvent
	{
		u64 nCycle;
		u16 nAddr;
		u8 nData;
	};
	static constexpr size_t kWEventFifoSize = 4096;
	mu50spsc::TSpscQueue<TWEvent, kWEventFifoSize> m_WEventFifo;
	size_t m_nWEventFifoCap;
	// Audio-side shadow of last-written SWP values (full 0x000-0x7ff
	// window), updated on every push in both modes. Split-mode read
	// fallback (see OnH8SwpRead): for the measured pattern (reads
	// always return the last-written value, M0 80,657/80,657) the
	// shadow is exact, and it is audio-local, so no live-state race
	// and no tear window between the two bytes of a 16-bit bus read
	// (the hook is per-byte; the seqlock retry the plan suggested
	// is subsumed by this -- there is no live read left to tear).
	u8 m_SwpShadow[2048];
	struct TNativeFrame
	{
		s16 nLeft;
		s16 nRight;
	};
	CRingBuffer<TNativeFrame, 16384> m_NativeRing;
	// M2 split-mode published blocks (SPSC: audio publishes, SWP
	// pops). nFifoSnap is the W-FIFO in-index at publish time: the
	// SWP core drains exactly up to it, so a block's render applies
	// exactly that block's writes even when the audio core has run
	// ahead (index bound, not cycle bound -- exact by construction).
	// nSeq is the block's monotonic publish sequence (M3): the
	// audio thread records the last aborted block's seq in
	// m_nAbortSeq, and the SWP core skips frames of blocks with
	// seq <= abortSeq (park-abort cleanup, see PCMTaskIteration).
	struct TPublishedBlock
	{
		size_t nFifoSnap;
		size_t nFrames;
		u64 nSeq;
	};
	mu50spsc::TSpscQueue<TPublishedBlock, 16> m_PublishedBlocks;
	// M2 MIDI staging (split mode only): MIDI-thread produces,
	// audio drains at each AdvanceH8. Kind 0 = SysEx byte (nB0),
	// 1 = short message (nB0/nB1/nB2), 2 = AllSoundOff control.
	// Staging (not a spinlock around the H8's SCI queue) is the
	// M2 threading shape: the H8 object stays single-threaded
	// (audio only, zero H8 changes, exact MIDI timing preserved --
	// queue_midi_byte still runs on the audio thread with the same
	// cycles_ value), and MIDI handlers never take m_Lock.
	struct TMidiStage
	{
		u8 nKind;
		u8 nB0;
		u8 nB1;
		u8 nB2;
	};
	mu50spsc::TSpscQueue<TMidiStage, 1024> m_MidiStage;
	// Cross-thread counters (M2/M3 diag names): __atomic access
	// everywhere. Plain u64 fields + builtins, no <atomic>, so this
	// header stays Circle-compatible. ring drops must stay 0 and
	// wq_full_waits must stay 0 at E1M1 load (M2 turns M1's
	// synchronous full-FIFO drains into backpressure waits).
	u64 m_nRingDrops;
	u64 m_nWqFullWaits;
	u64 m_nBlockWaits;
	u64 m_nStageDrops;
	u64 m_nRenderedEnqueued; // SWP: frames successfully enqueued
	u64 m_nConsumed;         // audio: frames dequeued
	u64 m_nPublishedCycles;  // audio: H8 cycles at last publish
	// M3 split diag + true underruns (__atomic access everywhere):
	// m_nUnderrunFrames counts every ring-empty-at-pull silence
	// insertion (split pull shortfall + park-abort silence +
	// single-core short pull, which M1 mislabeled a ring drop --
	// drops are enqueue-side only). It is cumulative; UpdateLCD
	// reports the per-window delta on the window line. SWP render
	// time and ring high-water feed the split line's per-core pct
	// and max-lag fields. m_nAbortSeq is the park-abort generation
	// (0 = none); publish seqs are monotonic for the synth's
	// lifetime and are NEVER reset (not even by 0xFF), so a fresh
	// block can never compare <= an old abort.
	u64 m_nUnderrunFrames;
	u64 m_nSwpRenderUs;
	u64 m_nRingHighWater;
	u64 m_nAbortSeq;
	u64 m_nNextPublishSeq;  // audio-only
	u64 m_nLastPublishedSeq; // audio-only: seq of block just published
	bool m_bLastPublishOk; // audio-only: last publish succeeded
	bool m_bSplitMode;       // set before SWP thread starts, then RO
	bool m_bSwpActive;       // false = parked (__atomic)
	bool m_bSwpParkAck;      // SWP set (__atomic)
	bool m_bAudioBusy;       // audio in RenderResampledSplit (__atomic)
	s16 m_SwpScratch[2 * kNativeScratchFrames]; // SWP-thread only
	// Audio-side pending resample blocks (lag-2 pipeline: at most 2).
	struct TPendingBlock
	{
		size_t nBlockOut;
		double dBlockStart;
		size_t nNativeRender;
		size_t nOutDone;
		u64 nSeq;
		// M3: false when the publish was skipped (queue full while
		// parked): no frames will ever arrive for this block, so
		// the pull silence-fills without waiting.
		bool bPublished;
	};
	TPendingBlock m_Pending[2];
	size_t m_nPendingCount;

	// M3: SWP-side native frame enqueue with park-abort skip.
	// Frames of blocks with seq <= m_nAbortSeq are dropped silently
	// (never counted as enqueued): without this, the tail of a
	// block the audio thread aborted would land in the ring after
	// the audio thread moved on and poison the next block's pull.
	// Returns true if the frame was enqueued.
	bool EnqueueSwpFrame(u64 nSeq, const TNativeFrame& Frame);

	// M3: refresh the ring-fill high-water after a rendered block
	// (SWP-thread only). The max over the window is the split
	// line's max-lag figure.
	void UpdateRingHighWater();
};

#endif
