//
// nukedsc55synth.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
// Copyright (C) 2020-2023 Dale Whinham <daleyo@gmail.com>
//
// This file is part of mt32-pi.
//
// mt32-pi is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
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

#ifndef _nukedsc55synth_h
#define _nukedsc55synth_h

#include <circle/types.h>

#include "ringbuffer.h"
#include "synth/synthbase.h"
#include "utility.h"

// The Nuked-SC55 backend classes are only forward-declared here, rather than
// including the backend headers: the backend requires C++20, while the
// mt32-pi kernel is built as C++14. Only nukedsc55synth.cpp includes the real
// headers, and it is compiled as C++20 (see Kernel.mk).
// The split state is declared at global scope on purpose: a nested forward
// declaration inside the class would shadow this type and break the .cpp's
// use of it.
class Emulator;
struct RomsetInfo;
struct ThreadSplitStateCircle;

class CNukedSC55Synth : public CSynthBase
{
public:
	#define ENUM_NUKEDH8ENGINE(ENUM) \
		ENUM(Decoder2, decoder2)     \
		ENUM(Legacy, legacy)         \
		ENUM(JIT, jit)

	CONFIG_ENUM(TH8Engine, ENUM_NUKEDH8ENGINE);

	CNukedSC55Synth(unsigned nSampleRate, float nGain, bool bOversampling, TH8Engine H8Engine, bool bSplit);
	virtual ~CNukedSC55Synth() override;

	// Dual-core split: true while the PCM task (core 3) may be
	// rendering for this synth (split configured on, MK1 romset,
	// emulator initialized and split state activated). The PCM task
	// in CMT32Pi gates on this; when false it yields.
	bool SplitActive() const { return m_bSplitActive; }

	// Split-state diag line: config flag, active flag, state
	// pointer, g_splitT match, PCM idle flag, model, sample-ring
	// fill and drops. Safe from any task: reads only volatile
	// flags/counters (DMB-ordered) and writes one diag line (never
	// under any other lock -- callers must not hold m_Lock; the
	// reset path calls it after releasing m_Lock).
	void LogSplitState(const char* pTag);

	// Sample-ring fill level (diagnostic snapshot, core-2
	// AudioTask heartbeat). The fill counter is maintained with
	// atomic add/sub (core 3 enqueues, core 2 dequeues); a stale
	// read is at most one batch out of date, never a torn value.

	// One PCM-task iteration (core 3): render one bounded batch of
	// ticks (at most 128, capped at published-800) under the raw
	// lock, then publish. Yields when there is no work. No-op when
	// the split is inactive (it parks the task and acks the stop
	// handshake instead).
	void PCMTaskIteration();

	// Synth-switch split control (called by CMT32Pi::SwitchSynth,
	// core 0): park the PCM task when switching away from this
	// synth, reactivate it (ring cleared) when switching back.
	// Each takes m_Lock, so no RenderFrames call can be in flight
	// across the transition. No-ops when the split was never
	// eligible (flag off, non-MK1, init failed) or, for activation,
	// after a park-timeout wedge (see m_bSplitWedged).
	void SplitActivate();
	void SplitDeactivate();

	// Cycle the H8 execution path (Button 2 while this synth is
	// active). Takes effect between instructions; returns the config
	// string name of the newly selected engine.
	const char* CycleH8Engine();
	static const char* H8EngineName(TH8Engine Engine);

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

	const char* GetModelName() const { return m_ModelName; }
	const char* GetROMVersion() const { return m_ROMVersion; }

private:
	// Dual-core split: native-rate frames produced by the emulator
	// are pushed into this ring by the sample callback and consumed
	// by the resampler in RenderFrames(). In split mode the callback
	// runs on core 3 (PCM task), outside m_Lock; in single-threaded
	// mode it runs inside Render() under m_Lock, as before. The ring
	// itself is lock-protected (CRingBuffer); the fill counter is a
	// single-producer / single-consumer volatile (core 3 enqueues,
	// core 2 dequeues), read only as a loop bound -- a stale-low read
	// just steps the MCU a little more, never short-reads, because
	// the dequeue count is the ground truth at drain time.
	// Size 16384 (as the old float ring): covers the priming burst
	// (~6400 frames) plus one AudioTask chunk, with wide headroom.
	struct TSplitSample
	{
		float nLeft;
		float nRight;
	};
	static constexpr size_t SplitRingFrames = 16384;

	// Backend split state (global ThreadSplitStateCircle, defined in
	// the backend header and forward-declared above). Owned by this
	// synth, created on split activation, deleted in the destructor.
	// The backend's g_splitT points at it while the split is active.
	::ThreadSplitStateCircle* m_pSplitState;

	// Split handshake flags (volatile, DMB-ordered at the handoff
	// points in the .cpp): m_bSplitActive gates the PCM task;
	// m_bPCMIdle acks that the PCM task has parked (used by the
	// reset/teardown stop sequence, when the emulator is about to
	// be reinitialized while core 3 might be mid-render).
	volatile bool m_bSplitActive;
	volatile bool m_bPCMIdle;

	// Set when a park wait times out: core 3 is wedged inside a
	// batch and must never be reset or reactivated under (that
	// would corrupt the backend under a live core). The only
	// recovery is a reboot. Never cleared once set.
	bool m_bSplitWedged;

	// True when this synth may run split at all: config flag on
	// AND an MK1 romset AND Initialize() succeeded. Decided once
	// at Initialize(); the switch paths consult it.
	bool m_bSplitEligible;

	void OnSample(s32 nLeft, s32 nRight);
	size_t RenderFrames(float* pOutBuffer, size_t nFrames);

	// Split lifecycle: stop the PCM task and clear g_splitT (waits
	// for the PCM idle ack, bounded; returns false on timeout, in
	// which case the caller must NOT reset or reactivate -- see
	// m_bSplitWedged; no-op when inactive), and (re)activate the
	// split from a live pcm_t (after priming, or after a reset).
	// bClearRing discards queued audio (reset path); activation
	// keeps the priming frames (continuous boot audio). Refuses
	// to activate when wedged.
	bool StopSplitPCM();
	void InitSplitState(bool bClearRing);

	// Core-3 diag heartbeat line: gated on the
	// 5 s diag interval, shared by the idle and work branches of
	// PCMTaskIteration. Runs with no lock held (never inside the
	// raw lock or a render batch).

	Emulator* m_pEmulator;
	RomsetInfo* m_pRomsetInfo;

	float m_nGain;
	bool m_bOversampling;
	TH8Engine m_H8Engine;
	bool m_bSplit;
	u8 m_nVolume;

	unsigned m_nNativeRate;
	double m_dResampleRatio;	// Native frames consumed per output frame
	double m_dReadFrac;		// Fractional carry of the streaming resampler
	volatile u64 m_nWrittenFrames;	// Total native frames produced (callback)
	volatile u64 m_nLastActiveFrame;	// Native frame index of the last above-threshold sample

	// Streaming resampler window: the two queued frames bracketing
	// the next output position (m_dReadFrac relative to the first).
	// Invalid until primed from the ring (first call, or after an
	// underrun); a failed prime means the whole call is silence.
	TSplitSample m_SampleA;
	TSplitSample m_SampleB;
	bool m_bWindowValid;

	CRingBuffer<TSplitSample, SplitRingFrames> m_SampleRing;
	volatile size_t m_nSampleRingFill;
	volatile u64 m_nSampleRingDrops;	// Enqueue failures (must stay 0)

	// Render timing statistics for the diagnostic log; accumulated in
	// RenderFrames() (audio context, brief stats lock) and flushed
	// periodically from UpdateLCD() when [diagnostics] is enabled.
	struct TRenderStats
	{
		u64 nCalls          = 0;
		u64 nFrames         = 0;
		u64 nTotalUs        = 0;
		u64 nMaxUs          = 0;
		u64 nMisses         = 0;
		u64 nUnderrunFrames = 0;
		u64 nWindowStartUs  = 0;
	};
	TRenderStats m_Stats;
	CSpinLock m_StatsLock;

	// Backend profiling snapshots (see nuked_profile.h in the backend):
	// cumulative counters are read and diffed at each diag flush.
	u64 m_nLastProfileTotalUs = 0;
	u64 m_nLastProfilePCMUs   = 0;
	u64 m_nLastProfileInsn    = 0;

	// Split diag: window accumulator for core-2 MCU stepping time
	// (reset at each flush, like m_Stats), plus last-flush snapshots
	// of the backend's cumulative split counters (diffed per window,
	// like the profile line). Lag peaks are reported as-is (peaks
	// since activation).
	u64 m_nMCUUs = 0;
	u64 m_nLastSplitLockWaits = 0;
	u64 m_nLastSplitLockContended = 0;
	u64 m_nLastSplitLockWaitUs = 0;
	u64 m_nLastSplitPublishes = 0;
	u64 m_nLastSplitTicks = 0;
	u64 m_nLastSplitIrq0Folded = 0;
	u64 m_nLastSplitPCMUs = 0;

	// Sounding-note tracking for power management (core 0 only:
	// written by the MIDI path, read by IsActive, both on MainTask,
	// so no lock is needed). A held or pedal-sustained note keeps
	// the synth "active" even through a quiet passage, so the clock
	// cannot drop to power-save mid-performance; silence with no
	// notes held still idles normally. Reset by AllSoundOff and by
	// the MIDI system-reset path.
	unsigned m_nHeldNotes = 0;
	bool m_bSustain = false;
	bool m_bSustainedNotes = false;


	char m_ModelName[16];
	char m_ROMVersion[16];
};

#endif
