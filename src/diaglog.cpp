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

CDiagLog::CDiagLog()
	: m_bEnabled(false),
	  m_nIntervalSeconds(5),
	  m_Lock(TASK_LEVEL)
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
	LogLine("==== diag session start (interval %u s) ====", m_nIntervalSeconds);
}

void CDiagLog::LogLine(const char* pFormat, ...)
{
	if (!m_bEnabled)
		return;

	char Buffer[256];
	const u64 nMs = CTimer::Get()->GetClockTicks() / 1000;
	int nLength = snprintf(Buffer, sizeof(Buffer), "t=%llu ", static_cast<unsigned long long>(nMs));
	if (nLength < 0)
		return;

	va_list Args;
	va_start(Args, pFormat);
	nLength += vsnprintf(Buffer + nLength, sizeof(Buffer) - static_cast<size_t>(nLength), pFormat, Args);
	va_end(Args);

	if (nLength > static_cast<int>(sizeof(Buffer)) - 2)
		nLength = static_cast<int>(sizeof(Buffer)) - 2;
	Buffer[nLength++] = '\n';
	Buffer[nLength]   = '\0';

	m_Lock.Acquire();
	UINT nWritten = 0;
	f_write(&m_File, Buffer, static_cast<UINT>(nLength), &nWritten);
	f_sync(&m_File);
	m_Lock.Release();
}
