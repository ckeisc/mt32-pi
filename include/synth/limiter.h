// limiter.h
//
// Lookahead limiter for mt32-pi's shared audio output stage.
//
// The limiter sits in CMT32Pi::AudioTask(), after the active synth's
// Render(float) and before the float-to-24-bit conversion, so it covers
// every engine (Nuked, MU50, EmuSC, SoundFont) on every song with no
// per-engine and no per-song tuning. It is deliberately Circle-free so
// the same code compiles on the host for verification.
//
// Why it exists: the AudioTask path has no clamp anywhere -- the synths'
// float Render paths are unclamped and the float-to-24-bit conversion
// multiplies straight into the 24-bit field. A hot song at gain 1.0
// therefore pushes over-full-scale floats into the 24-bit stage raw
// (overflow/wrap, heard as harsh pops), while gain 0.5 stays clean.
// This limiter guarantees the stream never exceeds its ceiling, so no
// song can wrap, whatever the gain.
//
// Design (fixed, no config key -- threshold and release are set once):
//   * Ceiling 0.99, just under full scale. Below the ceiling the gain
//     is exactly 1.0, so quiet material is bit-transparent (modulo the
//     fixed lookahead delay).
//   * Lookahead 240 frames (~5 ms at 48 kHz): the audio is delayed
//     while the detector watches the incoming peaks, so the gain can
//     ramp down BEFORE a peak arrives instead of reacting after it.
//   * Detector: peak of max(|L|, |R|) per frame; gain LINKED (one gain
//     for both channels), so the stereo image never shifts.
//   * Attack: when the window requires a lower gain, the gain ramps
//     linearly toward it over the lookahead window (fixed steps, no
//     single-sample steps, so no clicks). A strictly lower target
//     restarts the ramp from the current gain.
//   * Release: when the window allows a higher gain, the gain rises
//     exponentially toward it with a 4800-frame time constant
//     (~100 ms at 48 kHz): g += (target - g) / 4800.
//   * Safety cap: the gain applied to an exiting frame is additionally
//     capped by that frame's own ceiling requirement. In normal
//     operation the ramp completes on time and the cap never binds;
//     it exists only to guarantee the ceiling absolutely in the
//     pathological corner of continuously rising peaks, where a ramp
//     restart could otherwise lag by a few percent. Cap binds are
//     counted so tests can prove they do not happen on real material.
//
// State: the delay line, peak history, gain and ramp live in the
// limiter object, which AudioTask owns. AudioTask is the only caller
// (Core 2), so no locking is needed. The limiter is NOT reset on
// synth switches: resetting would inject 5 ms of silence, while
// letting the delay line drain naturally is inaudible.
//
// Units note: release/lookahead are frame-based, so the millisecond
// equivalents above assume 48 kHz; at other rates the frame counts
// are unchanged and the times scale accordingly.

#ifndef _limiter_h
#define _limiter_h

#include <stddef.h>
#include <stdint.h>

class CLimiter
{
public:
	static constexpr float Ceiling = 0.99f;
	static constexpr size_t LookaheadFrames = 240;
	static constexpr float ReleaseFrames = 4800.0f; // ~100 ms at 48 kHz

	CLimiter()
		: m_nDelayPos(0)
		, m_fGain(1.0f)
		, m_fRampTarget(1.0f)
		, m_fRampStep(0.0f)
		, m_bRampActive(false)
		, m_nLimitedFrames(0)
		, m_fMinGain(1.0f)
		, m_nCapBinds(0)
		, m_fMaxCapDeficit(0.0f)
	{
		for (size_t i = 0; i < LookaheadFrames; ++i)
		{
			m_DelayL[i] = 0.0f;
			m_DelayR[i] = 0.0f;
			m_DelayPeak[i] = 0.0f;
		}
	}

	// Process nFrames interleaved stereo float frames in place.
	// pGainTrace, when non-null, receives the gain applied to each
	// frame (test/diagnostics hook; null in production use).
	void Process(float* pBuffer, size_t nFrames, float* pGainTrace = nullptr)
	{
		for (size_t i = 0; i < nFrames; ++i)
		{
			const float fInL = pBuffer[i * 2];
			const float fInR = pBuffer[i * 2 + 1];
			const float fPeakIn = MaxAbs(fInL, fInR);

			// Window max: peaks of the frames currently in the
			// delay line plus the incoming frame.
			float fWindowMax = fPeakIn;
			for (size_t k = 0; k < LookaheadFrames; ++k)
			{
				if (m_DelayPeak[k] > fWindowMax)
					fWindowMax = m_DelayPeak[k];
			}
			float fTarget = 1.0f;
			if (fWindowMax > 0.0f)
			{
				fTarget = Ceiling / fWindowMax;
				if (fTarget > 1.0f)
					fTarget = 1.0f;
			}

			// Gain law: ramp down over the lookahead window on
			// attack, exponential release on the way back up.
			if (fTarget < m_fGain)
			{
				if (!m_bRampActive || fTarget < m_fRampTarget)
				{
					m_fRampTarget = fTarget;
					m_fRampStep = (fTarget - m_fGain) / static_cast<float>(LookaheadFrames);
					m_bRampActive = true;
				}
				m_fGain += m_fRampStep;
				if (m_fGain <= m_fRampTarget)
				{
					m_fGain = m_fRampTarget;
					m_bRampActive = false;
				}
			}
			else if (fTarget > m_fGain)
			{
				m_fGain += (fTarget - m_fGain) / ReleaseFrames;
				m_bRampActive = false;
			}
			// fTarget == m_fGain: hold.

			// Safety cap: never let the exiting frame exceed the
			// ceiling, even if a ramp restart lagged behind.
			const float fPeakOut = m_DelayPeak[m_nDelayPos];
			float fGainOut = m_fGain;
			if (fPeakOut > 0.0f)
			{
				const float fCap = Ceiling / fPeakOut;
				if (fCap < fGainOut)
				{
					const float fDeficit = fGainOut - fCap;
					if (fDeficit > m_fMaxCapDeficit)
						m_fMaxCapDeficit = fDeficit;
					fGainOut = fCap;
					++m_nCapBinds;
				}
			}

			pBuffer[i * 2] = m_DelayL[m_nDelayPos] * fGainOut;
			pBuffer[i * 2 + 1] = m_DelayR[m_nDelayPos] * fGainOut;
			if (pGainTrace != nullptr)
				pGainTrace[i] = fGainOut;

			if (fGainOut < 0.999f)
				++m_nLimitedFrames;
			if (fGainOut < m_fMinGain)
				m_fMinGain = fGainOut;

			m_DelayL[m_nDelayPos] = fInL;
			m_DelayR[m_nDelayPos] = fInR;
			m_DelayPeak[m_nDelayPos] = fPeakIn;
			m_nDelayPos = (m_nDelayPos + 1) % LookaheadFrames;
		}
	}

	uint64_t GetLimitedFrames() const { return m_nLimitedFrames; }
	// Minimum gain applied since the last ResetWindowStats(), as
	// milli-gain (1000 = untouched). Integer-only so the Pi diag
	// line needs no float printf.
	unsigned GetMinGainMilli() const
	{
		return static_cast<unsigned>(m_fMinGain * 1000.0f);
	}
	void ResetWindowStats() { m_fMinGain = 1.0f; }
	uint64_t GetCapBinds() const { return m_nCapBinds; }
	// Largest amount by which the safety cap ever trimmed the gain
	// (0 = never bound). Distinguishes float-rounding binds (~1e-7)
	// from genuine ramp lag.
	float GetMaxCapDeficit() const { return m_fMaxCapDeficit; }

private:
	static float MaxAbs(float fL, float fR)
	{
		float fA = fL < 0.0f ? -fL : fL;
		float fB = fR < 0.0f ? -fR : fR;
		return fA > fB ? fA : fB;
	}

	float m_DelayL[LookaheadFrames];
	float m_DelayR[LookaheadFrames];
	float m_DelayPeak[LookaheadFrames];
	size_t m_nDelayPos;
	float m_fGain;
	float m_fRampTarget;
	float m_fRampStep;
	bool m_bRampActive;
	uint64_t m_nLimitedFrames;
	float m_fMinGain;
	uint64_t m_nCapBinds;
	float m_fMaxCapDeficit;
};

#endif
