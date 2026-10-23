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

#ifndef _diaglog_h
#define _diaglog_h

#include <circle/spinlock.h>
#include <circle/types.h>
#include <fatfs/ff.h>

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
	void LogLine(const char* pFormat, ...);

private:
	CDiagLog();

	bool m_bEnabled;
	unsigned m_nIntervalSeconds;
	FIL m_File;
	CSpinLock m_Lock;
};

#endif
