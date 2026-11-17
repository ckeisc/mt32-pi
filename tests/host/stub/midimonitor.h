// Host stub for "midimonitor.h": monitor state is not under test.
#ifndef ENGSTUB_MIDIMONITOR_H
#define ENGSTUB_MIDIMONITOR_H
#include <circle/types.h>
class CMIDIMonitor {
public:
  void OnShortMessage(u32) {}
  void AllNotesOff() {}
  void GetChannelLevels(unsigned, float *pCh, float *pPeak, int) {
    for (int i = 0; i < 16; i++) { pCh[i] = 0.0f; pPeak[i] = 0.0f; }
  }
};
#endif
