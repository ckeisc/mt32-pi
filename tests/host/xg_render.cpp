// xg_render -- render the first N seconds of a MIDI file through the real
// MU50 engine (CMU50Synth + H8 firmware + libmu50) on the host, single-core
// mode, with a selectable filter mode. Modelled on eng_test.cpp.
// Ported into the repo (tests/host) for PR #15 round 2, issue 1: the
// bit-identical split-vs-single gates and the tiny-queue stress were
// previously run from a scratch copy outside the repo. Additions over
// the scratch version: an optional FIFO-cap argument (SetWEventFifoCapForTest,
// 0 = full cap) for the tiny-queue stress, and wq_full_waits in the DONE
// line so the stress numbers are printed by the harness itself.
// Usage: xg_render <events.txt> <out.pcm> <seconds> <filter0277 0|1> [bootsec]
//   events.txt: lines "<frame> SHORT <status> <d1> <d2>" or "<frame> SYSEX <hex>"
//   out.pcm: raw s16 stereo interleaved at 48 kHz.
//   bootsec: seconds of silence rendered before the first event is fed,
//   to let the H8 firmware finish booting (default 10). The MIDI clock
//   starts after the boot lead-in.
//   split 0|1 (default 0); cap: W-event FIFO cap for the stress (default 0).
// ROMs are found via the engstub fatfs mapping (SD:/roms/yamaha -> $HOME/roms/yamaha).
// Events files are generated from a MIDI with midi_to_events.py in this dir.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
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
  if (argc < 5) {
    std::fprintf(stderr, "usage: xg_render <events.txt> <out.pcm> <seconds> <filter0277 0|1> [bootsec] [split 0|1] [cap 0]\n");
    return 2;
  }
  const char *evfile = argv[1];
  const char *outfile = argv[2];
  const double seconds = std::atof(argv[3]);
  const bool filter0277 = std::atoi(argv[4]) != 0;
  const double bootsec = argc > 5 ? std::atof(argv[5]) : 10.0;
  const bool split = argc > 6 ? std::atoi(argv[6]) != 0 : false;
  const unsigned cap = argc > 7 ? static_cast<unsigned>(std::atoi(argv[7])) : 0;
  const unsigned boot_frames = (unsigned)(bootsec * 48000.0);

  FILE *evf = std::fopen(evfile, "r");
  if (!evf) { std::fprintf(stderr, "cannot open %s\n", evfile); return 1; }
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
  std::fprintf(stderr, "loaded %zu events, filter0277=%d, seconds=%.1f, boot=%.1fs, split=%d, cap=%u\n",
               evs.size(), filter0277 ? 1 : 0, seconds, bootsec, split ? 1 : 0, cap);
  // Offset all event frames by the boot lead-in so the firmware is up
  // before the first event arrives.
  for (auto &e : evs) e.frame += boot_frames;

  FILE *pcmfile = std::fopen(outfile, "wb");
  if (!pcmfile) { std::fprintf(stderr, "cannot open %s\n", outfile); return 1; }
  FILE *logfile = std::fopen("/dev/null", "w");
  CDiagLog::SetLogFile(logfile);

  CMU50Synth synth(48000, 1.0f, TMU50Resampler::Cubic);
  if (split) synth.SetSplitMode(true);
  if (cap != 0) synth.SetWEventFifoCapForTest(cap);
  if (!synth.Initialize()) {
    std::fprintf(stderr, "Initialize failed\n");
    return 1;
  }
  synth.SetFilter0277(filter0277);

  // Split mode: SWP worker thread renders blocks on demand.
  std::atomic<bool> stop{false};
  std::thread worker;
  if (split) {
    worker = std::thread([&] {
      while (!stop.load(std::memory_order_acquire)) {
        synth.PCMTaskIteration();
        std::this_thread::yield();
      }
    });
  }

  const unsigned kTotalFrames = boot_frames + (unsigned)(seconds * 48000.0);
  const unsigned kBlock = 512;
  unsigned done = 0, fed = 0;
  int peak = 0;
  double sumsq = 0.0;
  unsigned nsamp = 0;
  std::vector<s16> s16buf(kBlock * 2);
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
    for (unsigned i = 0; i < n * 2; i++) {
      float v = g_FloatBuf[i];
      if (v < -1.0f) v = -1.0f;
      if (v > 1.0f) v = 1.0f;
      s16 s = static_cast<s16>(v * 32767.0f);
      s16buf[i] = s;
      int a = s < 0 ? -s : s;
      if (a > peak) peak = a;
      sumsq += (double)s * (double)s;
      nsamp++;
    }
    std::fwrite(s16buf.data(), sizeof(s16), n * 2, pcmfile);
    done += n;
  }
  CLCD lcd;
  synth.UpdateLCD(lcd, 0);
  std::fprintf(stderr, "DONE frames=%u fed=%u peak=%d rms=%.1f active=%d waits=%llu\n",
               done, fed, peak,
               nsamp ? std::sqrt(sumsq / nsamp) : 0.0,
               synth.IsActive() ? 1 : 0,
               (unsigned long long)synth.GetWqFullWaits());
  if (split) {
    synth.ParkSwp();
    stop.store(true, std::memory_order_release);
    if (worker.joinable()) worker.join();
  }
  std::fclose(pcmfile);
  std::fclose(logfile);
  return 0;
}
