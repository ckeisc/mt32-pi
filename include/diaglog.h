//
// diaglog.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
//
// Diagnostic log written to a file on the SD card (mt32pi-diag.log),
// gated by the [diagnostics] section of mt32-pi.cfg. When disabled, no
// file is opened and LogLine() is a no-op. Lines are timestamped with
// milliseconds since boot and synced after every write, so the card can
// be pulled at any time without losing records.
//
// Item B (limiter log off core 2): LogLine() used to do f_write +
// f_sync (FatFs flush to SD) on the calling core -- in the real-time
// audio path on core 2 (AudioTask's limiter window line). Now LogLine()
// only formats the line and pushes it into a lock-free SPSC ring
// (producer: any core, serialized by m_RingLock for the brief push;
// consumer: core 1, UITask, via Drain()). Drain() performs the
// f_write + f_sync on core 1, so the audio core never touches the SD
// card. Line content is byte-identical to the synchronous path; only
// the core doing the SD write changed. A full ring drops the line
// (counted in m_nDropped) instead of blocking the audio path.
//

#ifndef _diaglog_h
#define _diaglog_h

#include <circle/spinlock.h>
#include <circle/types.h>
#include <fatfs/ff.h>

#include "synth/mu50spsc.h"

class CDiagLog
{
public:
	static CDiagLog& Get();

	// Call once after the config has been loaded and the SD card is
	// mounted. Opens the log file if diagnostics are enabled.
	void Initialize();

	bool IsEnabled() const { return m_bEnabled; }
	unsigned IntervalSeconds() const { return m_nIntervalSeconds; }

	// Append one line (prefixed with "t=<ms since boot>") and sync.
	// Formats the line and enqueues it for Drain(); no SD I/O on
	// the calling core. If the ring is full the line is dropped
	// (counted) instead of blocking.
	void LogLine(const char* pFormat, ...);

	// Drain the ring to the SD file (f_write + f_sync under m_Lock).
	// Called from core 1 (CMT32Pi::UITask) once per iteration.
	// No-op when diagnostics are disabled.
	void Drain();

	// Lines dropped because the ring was full (diagnostic counter).
	u64 GetDropped() const { return m_nDropped; }

private:
	CDiagLog();

	// One pre-formatted log line, as LogLine() produces it (same
	// "t=<ms> ..." text, including the trailing newline).
	struct TDiagLine
	{
		char sText[256];
		u16 nLength;
	};

	bool m_bEnabled;
	unsigned m_nIntervalSeconds;
	FIL m_File;
	CSpinLock m_Lock;
	// Ring (item B): 16 slots, 15 usable (SPSC full convention).
	// Producers (any core) are serialized by m_RingLock for the
	// brief push only -- the lock is never held across SD I/O, so
	// the audio core cannot spin on a core-1 SD write. The consumer
	// (core 1, Drain) uses the queue's lock-free TryPop.
	mu50spsc::TSpscQueue<TDiagLine, 16> m_Ring;
	CSpinLock m_RingLock;
	volatile u64 m_nDropped;
};

#endif
