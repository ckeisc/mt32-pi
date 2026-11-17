//
// diaglog.cpp
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
//
// See diaglog.h.
//

#include "diaglog.h"

#include <stdarg.h>
#include <stdio.h>

#include <circle/timer.h>

#include "config.h"

#ifndef MT32_PI_VERSION
#define MT32_PI_VERSION "<unknown>"
#endif

CDiagLog::CDiagLog()
	: m_bEnabled(false),
	  m_nIntervalSeconds(5),
	  m_Lock(TASK_LEVEL),
	  m_RingLock(TASK_LEVEL),
	  m_nDropped(0)
{
}

CDiagLog& CDiagLog::Get()
{
	static CDiagLog s_Instance;
	return s_Instance;
}

void CDiagLog::Initialize()
{
	CConfig* pConfig = CConfig::Get();
	if (!pConfig || !pConfig->DiagnosticsEnabled)
		return;

	if (pConfig->DiagnosticsIntervalSeconds > 0)
		m_nIntervalSeconds = static_cast<unsigned>(pConfig->DiagnosticsIntervalSeconds);

	if (f_open(&m_File, "SD:/mt32pi-diag.log", FA_WRITE | FA_OPEN_APPEND) != FR_OK)
		return;

	m_bEnabled = true;
	// Identify the kernel in the log: version (git describe), build
	// date/time and compiler, so a log can always be traced back to
	// the exact binary that produced it.
	LogLine("==== diag session start (interval %u s) ==== kernel=%s built=%s %s gcc=%s",
	        m_nIntervalSeconds, MT32_PI_VERSION, __DATE__, __TIME__, __VERSION__);
}

void CDiagLog::LogLine(const char* pFormat, ...)
{
	if (!m_bEnabled)
		return;

	// Format exactly as before (same "t=<ms> " prefix, same 256-byte
	// cap, same trailing newline) -- only the destination changed:
	// the ring, not the SD file. No f_write / f_sync here, so the
	// calling core (notably core 2, the audio path) never touches
	// the SD card.
	TDiagLine Line;
	const u64 nMs = CTimer::Get()->GetClockTicks() / 1000;
	int nLength = snprintf(Line.sText, sizeof(Line.sText), "t=%llu ", static_cast<unsigned long long>(nMs));
	if (nLength < 0)
		return;

	va_list Args;
	va_start(Args, pFormat);
	nLength += vsnprintf(Line.sText + nLength, sizeof(Line.sText) - static_cast<size_t>(nLength), pFormat, Args);
	va_end(Args);

	if (nLength > static_cast<int>(sizeof(Line.sText)) - 2)
		nLength = static_cast<int>(sizeof(Line.sText)) - 2;
	Line.sText[nLength++] = '\n';
	Line.sText[nLength]   = '\0';
	Line.nLength = static_cast<u16>(nLength);

	// Push under the ring lock: LogLine is called from several cores
	// (core 0 init/switch, core 2 audio), and the SPSC queue assumes
	// a single producer -- the lock serializes pushes so that
	// assumption holds. It is held only for the memcpy + index bump
	// (microseconds), never across SD I/O, so the audio core cannot
	// block on a core-1 SD write. A full ring drops the line
	// (counted) instead of blocking the caller.
	m_RingLock.Acquire();
	const bool bPushed = m_Ring.TryPush(Line);
	m_RingLock.Release();
	if (!bPushed)
		__atomic_add_fetch(&m_nDropped, 1ULL, __ATOMIC_RELAXED);
}

void CDiagLog::Drain()
{
	if (!m_bEnabled)
		return;

	// Consumer (core 1): pop lines lock-free and write them to the
	// SD file. The file lock covers f_write + f_sync: an unlocked
	// f_sync concurrent with another core's f_write shares FatFs's
	// volume window on one FIL and can garble the log or corrupt
	// the volume. Only Drain() touches the file now (LogLine only
	// enqueues), so m_Lock is uncontended in practice; it is kept
	// as the FatFs guard. One f_sync per drain batch (after all
	// lines) preserves the "card can be pulled" durability to
	// within one UITask iteration.
	TDiagLine Line;
	bool bWrote = false;
	m_Lock.Acquire();
	while (m_Ring.TryPop(Line))
	{
		UINT nWritten = 0;
		f_write(&m_File, Line.sText, Line.nLength, &nWritten);
		bWrote = true;
	}
	if (bWrote)
		f_sync(&m_File);
	m_Lock.Release();
}
