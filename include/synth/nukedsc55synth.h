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

#include <atomic>
#include <circle/types.h>

#include "ringbuffer.h"
#include "synth/mu50spsc.h"
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

// Nuked resampler selection (cfg [nuked] resampler): kaiser is the
// 66-tap polyphase FIR (default, best quality); cubic is Catmull-Rom
// (4 taps, ~10x cheaper); linear is the original 2-tap lerp.
enum class TNukedResampler
{
	Kaiser,
	Cubic,
	Linear
};

class CNukedSC55Synth : public CSynthBase
{
public:
	#define ENUM_NUKEDH8ENGINE(ENUM) \
		ENUM(Decoder2, decoder2)     \
		ENUM(Legacy, legacy)         \
		ENUM(JIT, jit)

	CONFIG_ENUM(TH8Engine, ENUM_NUKEDH8ENGINE);

	CNukedSC55Synth(unsigned nSampleRate, float nGain, bool bOversampling, TNukedResampler Resampler, unsigned nChunkSize);
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
	bool PCMTaskIteration();

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
	static const char* H8EngineName(TH8Engine Engine);

	// CSynthBase
	virtual bool Initialize() override;
	virtual void HandleMIDIShortMessage(u32 nMessage) override;
	virtual void HandleMIDISysExMessage(const u8* pData, size_t nSize) override;
	virtual bool IsActive() override;
	virtual void AllSoundOff() override;
	virtual void SetMasterVolume(u8 nVolume) override;
	// Fade-in on synth switch: reset the applied volume to silence
	// so the next Render ramps up to the target over VolumeRampFrames
	// instead of starting at full volume mid-signal. Takes m_Lock.
	void ResetVolumeRamp() override;
	virtual size_t Render(s16* pOutBuffer, size_t nFrames) override;
	virtual size_t Render(float* pOutBuffer, size_t nFrames) override;

	// Dev side-by-side toggle (HW2): advance the resampler selection
	// (kaiser -> cubic -> linear) and return the EFFECTIVE resampler
	// name for the LCD (kaiser selection on a non-64k rate reports
	// cubic, which is what is actually heard).
	const char* CycleResampler();
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
	// Drain the staged-MIDI queue (see m_MidiStage): posts staged
	// bytes/messages to the emulator, updates note tracking and the
	// MIDI monitor, all on the audio thread. Called from
	// RenderFrames after acquiring m_Lock, before stepping.
	void DrainMidiStage();
	// Volume scale for the next output frame (m_nGain * applied
	// volume), advancing the ramp by one frame. Called exactly once
	// per output frame, in order, from RenderFrames.
	float NextVolumeScale();

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
	u8 m_nVolume;

	// Master-volume ramp: SetMasterVolume only stores the target
	// (m_nVolume); the applied volume chases it over ~5 ms so a
	// volume change is a smooth ramp, not a gain step at the next
	// Render call. The ramp is advanced once per output frame by
	// NextVolumeScale(), so it spans Render calls. Audio-thread
	// only (RenderFrames holds m_Lock); ResetVolumeRamp() also
	// takes m_Lock, so a synth switch can force a fade-in from
	// silence without racing the audio thread.
	static constexpr size_t VolumeRampFrames = 240; // ~5 ms at 48 kHz
	float m_fVolumeApplied;    // currently applied volume, 0.0-1.0
	float m_fVolumeRampTarget; // target the current ramp heads toward
	float m_fVolumeRampStep;   // per-frame step toward the target
	size_t m_nVolumeRampFrames; // frames remaining in the current ramp

	unsigned m_nNativeRate;
	// One DMA block, in output frames: half the configured audio
	// chunk size (256 -> 128). The Render wall-clock deadline is
	// capped at one DMA block; deriving it from the chunk size
	// keeps the cap correct if the chunk size ever changes
	// (Claude review 2026-11-16 round 3).
	unsigned m_nDMAblockFrames;
	double m_dResampleRatio;	// Native frames consumed per output frame
	double m_dReadFrac;		// Fractional carry of the streaming resampler
	volatile u64 m_nWrittenFrames;	// Total native frames produced (callback)
	volatile u64 m_nLastActiveFrame;	// Native frame index of the last above-threshold sample

	// Streaming resampler window: the two queued frames bracketing
	// the next output position (m_dReadFrac relative to the first).
	// Invalid until primed from the ring (first call only); an
	// underrun keeps the window (concealment) so the next call
	// resumes from where it left off. A failed prime means the
	// whole call is silence.
	TSplitSample m_SampleA;
	TSplitSample m_SampleB;
	bool m_bWindowValid;

	// Polyphase FIR resampler (64k -> 48k only; coefficients in
	// fir64k48k.h). History is circular (128 slots, interleaved
	// L/R floats); the newest sample fed sits at (m_FirFed-1)&127.
	// m_FirPhase is always m_FirOutIndex % 3; output n consumes
	// newest input m = (4n - phase) / 3, feeding from the ring
	// until m_FirFed > m.
	bool m_bFirActive;
	float m_FirHist[128 * 2];
	size_t m_FirFed;
	size_t m_FirOutIndex;
	unsigned m_FirPhase;
	void ResetFirState();
	// Derive the effective resampler from the selection + rates and
	// reset all resampler state so the newly active resampler primes
	// fresh. Caller must hold m_Lock once rendering has started
	// (Init runs pre-render, so it calls this unlocked).
	void ApplyResamplerSelection();

	// Cubic Catmull-Rom resampler window (4 samples bracketing the
	// output position, m_dReadFrac relative to m_CubicA). Primed
	// from the ring (first call only); an underrun keeps the window
	// (concealment). m_CubicPrev duplicates m_CubicA on prime (frac
	// starts at 0, so the first output is exactly m_CubicA regardless).
	TSplitSample m_CubicPrev;
	TSplitSample m_CubicA;
	TSplitSample m_CubicB;
	TSplitSample m_CubicNext;
	bool m_bCubicValid;
	bool m_bCubicActive;
	TNukedResampler m_Resampler;

	// Underrun concealment (pop-free playback): when the ring
	// starves mid-call, the output fades out over the last
	// ConcealFadeFrames produced frames instead of stepping to
	// hard zero, and the resampler state (read frac, window, FIR
	// history, cubic window) is KEPT so the next call resumes
	// from where it left off. On the next call after a starved
	// one, the first ConcealFadeFrames fade back in from zero.
	// m_bConcealed is true when the previous RenderFrames call
	// ended starved (faded out). 96 frames = 2 ms at 48 kHz.
	//
	// Clean mute (item F): under SUSTAINED overload the per-call
	// fade-out/fade-in above cycles every DAC block (128 frames
	// = 375 Hz), heard as a buzz. Instead, when an underrun
	// leaves the ring unable to fill even one requested block,
	// the synth latches m_bMuted: it fades out once (the entering
	// call's fade-out), then outputs pure zeros -- resampler
	// skipped, no fade-in/out -- until the ring recovers to
	// MuteRecoverFill (the prefill target, 512 native frames).
	// The exit fades back in once via the m_bConcealed mechanism
	// (m_bConcealed stays true while muted). Hysteresis: the low
	// threshold (fill < nNeed, ~256-341 native frames) is well
	// below the high threshold (512), so the mute does not flap.
	static constexpr size_t ConcealFadeFrames = 96;
	static constexpr size_t MuteRecoverFill = 512; // prefill target (mt32pi.cpp)
	bool m_bConcealed;
	bool m_bMuted;

	// Last output sample of the previous RenderFrames call
	// (left/right, after any fade/decay). On an underrun that
	// produces nothing (nDone == 0), the gap decays forward
	// from this value instead of stepping to zero -- the
	// previous block is already in the DAC queue and cannot be
	// faded, so the tail has to start from where the signal
	// actually was.
	float m_fLastOutL = 0.0f;
	float m_fLastOutR = 0.0f;

	// Prefill: step the emulator until the sample ring holds at
	// least nTargetFill native frames (or the step budget is
	// exhausted). Called from AudioTask when the DAC queue is
	// full, so idle time builds a cushion instead of spinning
	// on zero-frame Render calls. Returns the ring fill after
	// prefill. pfnShouldStop is polled during the prefill (see
	// synthbase.h); when it returns true the prefill stops early
	// so AudioTask can Render instead.
	size_t Prefill(size_t nTargetFill, bool (*pfnShouldStop)(void*) = nullptr, void* pStopContext = nullptr) override;

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
	u64 m_nLastSplitFifoFullWaits = 0;
	u64 m_nLastSplitFifoDrops = 0;
	u64 m_nLastSplitPCMUs = 0;
	u64 m_nLastSplitFifoResync = 0; // last-seen fifo_resync (item E)

	// Sounding-note tracking for power management: written by
	// DrainMidiStage on the audio thread (core 2) and read by
	// IsActive on core 0, so these are atomic -- the old comment
	// claiming "core 0 only, no lock is needed" predates the
	// lock-free MIDI staging, which moved the writes to core 2.
	// A held or pedal-sustained note keeps the synth "active"
	// even through a quiet passage, so the clock cannot drop to
	// power-save mid-performance; silence with no notes held
	// still idles normally. Reset by AllSoundOff and by the MIDI
	// system-reset path.
	std::atomic<unsigned> m_nHeldNotes{0};
	std::atomic<bool> m_bSustain{false};
	std::atomic<bool> m_bSustainedNotes{false};

	// Lock-free MIDI input (single-producer/single-consumer):
	// the MIDI thread (core 0) stages messages here without taking
	// m_Lock, and RenderFrames (core 2, audio thread) drains them
	// into the emulator before stepping. This removes the lock
	// contention where core 0 spun waiting for a whole Render to
	// finish before it could post MIDI. Mirrors MU50's m_MidiStage.
	// Kinds: 0 = one raw byte (SysEx), 1 = short message
	// (nB0=status, nB1=data1, nB2=data2), 2 = AllSoundOff.
	struct TMidiStage
	{
		u8 nKind;
		u8 nB0;
		u8 nB1;
		u8 nB2;
	};
	mu50spsc::TSpscQueue<TMidiStage, 4096> m_MidiStage;
	// Staged entries dropped because the queue was full (diagnostic;
	// the queue holds 4095 entries, far above any MIDI burst).
	// Reported as stage_drops in the window diag line; any
	// non-zero value means a burst was truncated.
	u32 m_nStageDrops = 0;


	char m_ModelName[16];
	char m_ROMVersion[16];
};

#endif
