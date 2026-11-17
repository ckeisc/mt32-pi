// reg_scan -- split-mode register readback scan (PR #15 review
// item 7, Finding 7: the 0x2e/0x2f written-mask).
//
// The wrapper seeds its per-channel shadow with the MixerBlock's
// EFFECTIVE reset values (glo 0xff, pan 0xff for regs 0x2e/0x2f),
// because the key-on overlay needs them -- but libmu50's
// channel_regs reset those two registers to 0x00. Before the fix,
// a split-mode read of a never-written 0x2e/0x2f returned the
// shadow's 0xff; after the fix the written-mask (m_nGloPanWrittenMask)
// makes never-written 0x2e/0x2f read as 0x00, matching the core.
//
// The scan drives the real wrapper (CMU50Synth, split mode) with no
// MIDI fed, so the W-event FIFO is empty and every read goes through
// the shadow/mask path in OnH8SwpRead. It covers the full mapped
// voice-register set -- reg 0x08-0x0b and 0x20-0x37 (28 regs) x 32
// channels = 896 reads -- expecting the core reset values:
//   reg 0x08 -> 0x80, 0x20 -> 0x27, 0x21 -> 0xff (shadow seeds that
//   match the core reset), 0x2e -> 0x00, 0x2f -> 0x00 (the mask;
//   the shadow holds 0xff), every other mapped reg -> 0x00.
// Then it writes distinct values to all 64 (0x2e, 0x2f) pairs via
// OnH8SwpWrite and reads them back (pending-scan path), plus a
// 0x20/0x21 write/readback pair, expecting the written values.
//
// Needs the MU50 ROMs (SD:/roms/yamaha via the engstub fatfs
// mapping, same as xg_render): Initialize() loads them, but no
// audio is rendered, so the firmware never runs and no register
// has been written when the scan starts.
//
// History: the round-1 response cited a "902-register scan, 0
// mismatches" for this fix, but that scan's source was never put in
// the repo and its exact 902 set cannot be reconstructed (896 mapped
// voice regs + 6 unidentified). This test re-covers the same
// property over the full 896 mapped voice regs plus the 64
// write/readback pairs, and states its counts explicitly instead
// of claiming the lost 902.
#include <cstdio>
#include <cstdint>
#include <cstdlib>

#include "diaglog.h"
#include "lcd/lcd.h"
#include "synth/mu50synth.h"

namespace {
int g_failures = 0;

// rchan window decode, mirroring Synth::write_register:
// address = ((reg & 0x3e) << 5) | (reg & 1) | (voice << 1).
std::uint16_t ChanAddr(std::uint8_t reg, unsigned chan) {
    return static_cast<std::uint16_t>(((reg & 0x3e) << 5) | (reg & 1) | (chan << 1));
}

std::uint8_t ResetValue(std::uint8_t reg) {
    switch (reg) {
    case 0x08: return 0x80;
    case 0x20: return 0x27;
    case 0x21: return 0xff;
    case 0x2e: return 0x00; // mask: shadow holds 0xff
    case 0x2f: return 0x00; // mask: shadow holds 0xff
    default: return 0x00;
    }
}
} // namespace

int main() {
    FILE* logfile = std::fopen("/dev/null", "w");
    if (!logfile) { std::fprintf(stderr, "cannot open /dev/null\n"); return 2; }
    CDiagLog::SetLogFile(logfile);

    CMU50Synth synth(48000, 1.0f, TMU50Resampler::Cubic);
    synth.SetSplitMode(true);
    if (!synth.Initialize()) {
        std::fprintf(stderr, "Initialize failed (need MU50 ROMs at SD:/roms/yamaha)\n");
        return 2;
    }

    // Never-written scan: 28 mapped regs x 32 channels = 896 reads.
    static const std::uint8_t kRegs[] = {
        0x08, 0x09, 0x0a, 0x0b,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    };
    int mismatches = 0;
    int first_chan = -1, first_reg = -1;
    unsigned first_got = 0, first_want = 0;
    for (unsigned chan = 0; chan < 32; ++chan) {
        for (std::uint8_t reg : kRegs) {
            const std::uint8_t want = ResetValue(reg);
            const std::uint8_t got = synth.OnH8SwpRead(ChanAddr(reg, chan));
            if (got != want) {
                if (mismatches == 0) {
                    first_chan = static_cast<int>(chan);
                    first_reg = reg; first_got = got; first_want = want;
                }
                ++mismatches;
            }
        }
    }
    std::fprintf(stderr, "never-written scan: 896 reads, %d mismatches\n", mismatches);
    if (mismatches > 0) {
        std::fprintf(stderr, "first mismatch: chan %d reg 0x%02x got 0x%02x want 0x%02x\n",
                     first_chan, first_reg, first_got, first_want);
        ++g_failures;
    }

    // Write/readback: distinct values into all 64 (0x2e, 0x2f) pairs.
    int wr_mismatches = 0;
    for (unsigned chan = 0; chan < 32; ++chan) {
        const std::uint8_t glo = static_cast<std::uint8_t>(0x10 + chan);
        const std::uint8_t pan = static_cast<std::uint8_t>(0x80 + chan);
        synth.OnH8SwpWrite(0, ChanAddr(0x2e, chan), glo);
        synth.OnH8SwpWrite(0, ChanAddr(0x2f, chan), pan);
        if (synth.OnH8SwpRead(ChanAddr(0x2e, chan)) != glo) ++wr_mismatches;
        if (synth.OnH8SwpRead(ChanAddr(0x2f, chan)) != pan) ++wr_mismatches;
    }
    // Plus a 0x20/0x21 pair (filter info) on channel 7.
    synth.OnH8SwpWrite(0, ChanAddr(0x20, 7), 0x12);
    synth.OnH8SwpWrite(0, ChanAddr(0x21, 7), 0x34);
    if (synth.OnH8SwpRead(ChanAddr(0x20, 7)) != 0x12) ++wr_mismatches;
    if (synth.OnH8SwpRead(ChanAddr(0x21, 7)) != 0x34) ++wr_mismatches;
    std::fprintf(stderr, "write/readback: 66 pairs, %d mismatches\n", wr_mismatches);
    if (wr_mismatches > 0) ++g_failures;

    std::fclose(logfile);
    if (g_failures != 0) {
        std::fprintf(stderr, "reg_scan: FAIL (%d failures)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "reg_scan: PASS (896 never-written + 66 write/readback, 0 mismatches)\n");
    return 0;
}
