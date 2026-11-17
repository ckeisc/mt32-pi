// meg_offsets -- dump the firmware-programmed MEG delay-offset table
// (registers 0x180-0x1ff) after rendering the start of a MIDI file
// through the real MU50 engine on the host (single-core mode).
//
// The reverb final stage feeds back through delay taps indexed by
// offsets_[tap/3] (see Delay::get/put in swp00_meg.cpp): a put at
// tap W and a get at tap R form a delay of (offsets_[R/3] -
// offsets_[W/3]) samples. The left reverb channel uses put 0x48 /
// get 0x36 (indices 24/18); the proposed right-channel fix uses
// put 0x54 / get 0x42 (indices 28/22). This tool logs the table so
// the two delays can be compared before shipping the fix: if
// offsets_[22]-offsets_[28] is not similar in length to
// offsets_[18]-offsets_[24], the inferred taps are wrong and the
// fix must not ship.
//
// Usage: meg_offsets <events.txt> <seconds> [bootsec=10]
// Prints the 64 u16 offsets (index: value) plus the two delays.
// ROMs via the engstub fatfs mapping (SD:/roms/yamaha).
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

#include "diaglog.h"
#include "lcd/lcd.h"
#include "synth/mu50synth.h"

namespace {
float g_FloatBuf[1024];

struct Ev {
  unsigned frame;
  bool sysex;
  unsigned status, d1, d2;
  std::vector<unsigned char> bytes;
};

int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: meg_offsets <events.txt> <seconds> [bootsec]\n");
    return 2;
  }
  const char *evfile = argv[1];
  const double seconds = std::atof(argv[2]);
  const double bootsec = argc > 3 ? std::atof(argv[3]) : 10.0;
  const unsigned boot_frames = (unsigned)(bootsec * 48000.0);

  FILE *evf = std::fopen(evfile, "r");
  if (!evf) { std::fprintf(stderr, "cannot open %s\n", evf); return 1; }
  std::vector<Ev> evs;
  char line[4096];
  while (std::fgets(line, sizeof line, evf)) {
    unsigned frame;
    char kind[16];
    if (std::sscanf(line, "%u %15s", &frame, kind) != 2) continue;
    Ev e;
    e.frame = frame;
    if (std::strcmp(kind, "SHORT") == 0) {
      unsigned st, d1, d2;
      if (std::sscanf(line, "%u %*s %u %u %u", &frame, &st, &d1, &d2) != 4) continue;
      e.sysex = false; e.status = st; e.d1 = d1; e.d2 = d2;
    } else if (std::strcmp(kind, "SYSEX") == 0) {
      char *hex = std::strstr(line, "SYSEX");
      if (!hex) continue;
      hex += 5;
      e.sysex = true;
      for (char *p = hex; *p && *(p+1); p += 2) {
        int hi = hexval((unsigned char)*p), lo = hexval((unsigned char)*(p+1));
        if (hi < 0 || lo < 0) break;
        e.bytes.push_back((unsigned char)((hi << 4) | lo));
      }
      if (e.bytes.empty()) continue;
    } else {
      continue;
    }
    evs.push_back(e);
  }
  std::fclose(evf);
  for (auto &e : evs) e.frame += boot_frames;

  FILE *logfile = std::fopen("/dev/null", "w");
  CDiagLog::SetLogFile(logfile);

  CMU50Synth synth(48000, 1.0f, TMU50Resampler::Cubic);
  if (!synth.Initialize()) {
    std::fprintf(stderr, "Initialize failed\n");
    return 1;
  }
  synth.SetFilter0277(true);

  const unsigned kTotalFrames = boot_frames + (unsigned)(seconds * 48000.0);
  const unsigned kBlock = 512;
  unsigned done = 0, fed = 0;
  while (done < kTotalFrames) {
    while (fed < evs.size() && evs[fed].frame <= done) {
      if (evs[fed].sysex) {
        synth.HandleMIDISysExMessage(evs[fed].bytes.data(), evs[fed].bytes.size());
      } else {
        unsigned code = evs[fed].status | (evs[fed].d1 << 8) | (evs[fed].d2 << 16);
        synth.HandleMIDIShortMessage(code);
      }
      fed++;
    }
    const unsigned n = kTotalFrames - done < kBlock ? kTotalFrames - done : kBlock;
    synth.Render(g_FloatBuf, n);
    done += n;
  }
  std::fprintf(stderr, "rendered %u frames, fed %zu events\n", done, fed);

  // Dump the offset table: registers 0x180-0x1ff are 64 u16 entries.
  // Byte order (see MegState::write_offset/read_offset): the EVEN
  // register holds the HIGH byte, the odd register the LOW byte.
  // Single-core OnH8SwpRead drains the FIFO, then reads the live core.
  std::uint16_t off[64] = {};
  for (int i = 0; i < 64; ++i) {
    const unsigned hi = synth.OnH8SwpRead(static_cast<std::uint16_t>(0x180 + 2 * i));
    const unsigned lo = synth.OnH8SwpRead(static_cast<std::uint16_t>(0x180 + 2 * i + 1));
    off[i] = static_cast<std::uint16_t>((hi << 8) | lo);
  }
  for (int i = 0; i < 64; ++i)
    std::printf("off[%2d] = %5u (0x%04x)\n", i, off[i], off[i]);
  const int left_delay = static_cast<int>(off[18]) - static_cast<int>(off[24]);
  const int right_delay = static_cast<int>(off[22]) - static_cast<int>(off[28]);
  std::printf("left  delay (put 0x48 idx24 -> get 0x36 idx18): %d samples (%.2f ms @44.1k)\n",
              left_delay, left_delay / 44.1);
  std::printf("right delay (put 0x54 idx28 -> get 0x42 idx22): %d samples (%.2f ms @44.1k)\n",
              right_delay, right_delay / 44.1);
  std::fclose(logfile);
  return 0;
}
