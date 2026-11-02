// Semihosted A/B renderer: links the exact libmt32emu.a / libfluidsynth.a from a
// pim-32 kernel build and renders MIDI to raw float32 stereo, so GCC 11.3 and
// GCC 15.2 builds of the same code can be compared bit for bit under QEMU.
//
//   abrender fluid <sf2> <mid> <out.f32> [nofx|noreverb|nochorus]
//   abrender munt  <ctrl.rom> <pcm.rom> <mid> <out.f32> [setup.syx|setup.mid ...] [until=SECONDS] [solo=CHANNEL]
//
// Output: 48 kHz, interleaved stereo float32 (what mt32-pi's audio task gets).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>

#ifdef WITH_FLUID
#include <fluidsynth.h>
#endif
#ifdef WITH_MUNT
#include <mt32emu/mt32emu.h>
#endif

static const unsigned SampleRate = 48000;
static const unsigned Chunk      = 256;

struct Event
{
	double t;          // seconds
	uint32_t seq;      // stable order
	std::vector<uint8_t> data; // short message bytes, or full SysEx incl. F0..F7
};

static std::vector<uint8_t> ReadFile(const char* path)
{
	std::vector<uint8_t> v;
	FILE* f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
	fclose(f);
	return v;
}

static uint32_t ReadVLQ(const std::vector<uint8_t>& d, size_t& p)
{
	uint32_t v = 0;
	for (int i = 0; i < 4 && p < d.size(); ++i)
	{
		uint8_t b = d[p++];
		v = (v << 7) | (b & 0x7F);
		if (!(b & 0x80)) break;
	}
	return v;
}

// Minimal SMF type 0/1 reader with a global tempo map.
static std::vector<Event> ParseSMF(const std::vector<uint8_t>& d, uint32_t& seq)
{
	auto be32 = [&](size_t p) { return (uint32_t(d[p]) << 24) | (d[p + 1] << 16) | (d[p + 2] << 8) | d[p + 3]; };
	auto be16 = [&](size_t p) { return uint16_t((d[p] << 8) | d[p + 1]); };
	if (d.size() < 14 || memcmp(d.data(), "MThd", 4)) { fprintf(stderr, "not an SMF\n"); exit(2); }
	const uint16_t ntrk = be16(10), div = be16(12);
	if (div & 0x8000) { fprintf(stderr, "SMPTE division unsupported\n"); exit(2); }

	struct Raw { uint64_t tick; uint32_t seq; bool tempo; uint32_t usPerQ; std::vector<uint8_t> data; };
	std::vector<Raw> raw;
	size_t p = 8 + be32(4);
	for (unsigned t = 0; t < ntrk && p + 8 <= d.size(); ++t)
	{
		const uint32_t len = be32(p + 4);
		size_t q = p + 8, end = std::min(d.size(), q + len);
		uint64_t tick = 0;
		uint8_t running = 0;
		while (q < end)
		{
			tick += ReadVLQ(d, q);
			uint8_t st = d[q];
			if (st == 0xFF)
			{
				const uint8_t type = d[q + 1];
				q += 2;
				const uint32_t l = ReadVLQ(d, q);
				if (type == 0x51 && l == 3)
					raw.push_back({tick, seq++, true, uint32_t((d[q] << 16) | (d[q + 1] << 8) | d[q + 2]), {}});
				q += l;
				if (type == 0x2F) break;
			}
			else if (st == 0xF0 || st == 0xF7)
			{
				++q;
				const uint32_t l = ReadVLQ(d, q);
				std::vector<uint8_t> s;
				if (st == 0xF0) s.push_back(0xF0);
				s.insert(s.end(), d.begin() + q, d.begin() + q + l);
				q += l;
				if (!s.empty() && s[0] == 0xF0) raw.push_back({tick, seq++, false, 0, s});
			}
			else
			{
				if (st & 0x80) { running = st; ++q; }
				const uint8_t hi = running & 0xF0;
				const int n = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
				std::vector<uint8_t> m{running};
				for (int i = 0; i < n; ++i) m.push_back(d[q++]);
				raw.push_back({tick, seq++, false, 0, m});
			}
		}
		p = p + 8 + len;
	}
	std::stable_sort(raw.begin(), raw.end(), [](const Raw& a, const Raw& b) { return a.tick < b.tick; });

	std::vector<Event> ev;
	double us = 500000, tsec = 0;
	uint64_t last = 0;
	for (auto& r : raw)
	{
		tsec += double(r.tick - last) * us / div / 1e6;
		last = r.tick;
		if (r.tempo) us = r.usPerQ;
		else ev.push_back({tsec, r.seq, r.data});
	}
	return ev;
}

// A .syx file: concatenated F0..F7 messages, sent 50 ms apart.
static std::vector<Event> ParseSyx(const std::vector<uint8_t>& d, uint32_t& seq)
{
	std::vector<Event> ev;
	double t = 0;
	for (size_t i = 0; i < d.size(); ++i)
	{
		if (d[i] != 0xF0) continue;
		size_t j = i;
		while (j < d.size() && d[j] != 0xF7) ++j;
		if (j == d.size()) break;
		ev.push_back({t, seq++, std::vector<uint8_t>(d.begin() + i, d.begin() + j + 1)});
		t += 0.05;
		i = j;
	}
	return ev;
}

static bool EndsWith(const char* s, const char* suf)
{
	size_t a = strlen(s), b = strlen(suf);
	return a >= b && !strcasecmp(s + a - b, suf);
}

template <class RenderFn, class SendFn>
static void Run(std::vector<Event>& ev, double tail, FILE* out, RenderFn render, SendFn send)
{
	std::vector<float> buf(Chunk * 2);
	uint64_t pos = 0;
	size_t i = 0;
	const double endT = (ev.empty() ? 0 : ev.back().t) + tail;
	const uint64_t endPos = uint64_t(endT * SampleRate);
	while (pos < endPos)
	{
		while (i < ev.size() && uint64_t(ev[i].t * SampleRate) <= pos) send(ev[i++].data);
		uint64_t next = endPos;
		if (i < ev.size()) next = std::min<uint64_t>(next, uint64_t(ev[i].t * SampleRate));
		const unsigned n = unsigned(std::min<uint64_t>(Chunk, std::max<uint64_t>(1, next - pos)));
		render(buf.data(), n);
		fwrite(buf.data(), sizeof(float), n * 2, out);
		pos += n;
	}
}

#ifdef WITH_FLUID
extern "C"
{
void* fluid_alloc(size_t len) { return malloc(len); }
void* fluid_realloc(void* p, size_t len) { return realloc(p, len); }
void fluid_free(void* p) { free(p); }
FILE* fluid_file_open(const char* path, const char** errMsg)
{
	FILE* f = fopen(path, "rb");
	if (!f && errMsg) *errMsg = "Failed to open file";
	return f;
}
void* default_fopen(const char* path) { return fopen(path, "rb"); }
int default_fclose(void* h) { return fclose((FILE*)h) == 0 ? FLUID_OK : FLUID_FAILED; }
fluid_long_long_t default_ftell(void* h) { return ftell((FILE*)h); }
int safe_fread(void* buf, fluid_long_long_t count, void* h) { return fread(buf, count, 1, (FILE*)h) == 1 ? FLUID_OK : FLUID_FAILED; }
int safe_fseek(void* h, fluid_long_long_t ofs, int whence) { return fseek((FILE*)h, ofs, whence) == 0 ? FLUID_OK : FLUID_FAILED; }
}

static int MainFluid(int argc, char** argv)
{
	if (argc < 5) return 1;
	uint32_t seq = 0;
	auto ev = ParseSMF(ReadFile(argv[3]), seq);

	fluid_settings_t* s = new_fluid_settings();
	fluid_settings_setint(s, "synth.device-id", 0x10);
	fluid_settings_setnum(s, "synth.sample-rate", SampleRate);
	fluid_settings_setint(s, "synth.threadsafe-api", 0);
	fluid_settings_setstr(s, "synth.reverb.engine", "fdn");
	fluid_synth_t* syn = new_fluid_synth(s);
	// mt32-pi defaults (include/config.def)
	fluid_synth_set_polyphony(syn, 200);
	fluid_synth_set_gain(syn, 0.2f);
	fluid_synth_reverb_on(syn, -1, 1);
	fluid_synth_set_reverb_group_damp(syn, -1, 0.0);
	fluid_synth_set_reverb_group_level(syn, -1, 0.9);
	fluid_synth_set_reverb_group_roomsize(syn, -1, 0.2);
	fluid_synth_set_reverb_group_width(syn, -1, 0.5);
	fluid_synth_chorus_on(syn, -1, 1);
	fluid_synth_set_chorus_group_depth(syn, -1, 8.0);
	fluid_synth_set_chorus_group_level(syn, -1, 2.0);
	fluid_synth_set_chorus_group_nr(syn, -1, 3);
	fluid_synth_set_chorus_group_speed(syn, -1, 0.3);
	if (argc > 5 && !strcmp(argv[5], "nofx")) { fluid_synth_reverb_on(syn, -1, 0); fluid_synth_chorus_on(syn, -1, 0); }
	if (argc > 5 && !strcmp(argv[5], "noreverb")) fluid_synth_reverb_on(syn, -1, 0);
	if (argc > 5 && !strcmp(argv[5], "nochorus")) fluid_synth_chorus_on(syn, -1, 0);
	if (fluid_synth_sfload(syn, argv[2], 1) == FLUID_FAILED) { fprintf(stderr, "sfload failed\n"); return 3; }

	FILE* out = fopen(argv[4], "wb");
	Run(ev, 3.0, out,
	    [&](float* b, unsigned n) { fluid_synth_write_float(syn, n, b, 0, 2, b, 1, 2); },
	    [&](const std::vector<uint8_t>& m) {
		    if (m[0] == 0xF0) { fluid_synth_sysex(syn, (const char*)m.data() + 1, int(m.size()) - 2, nullptr, nullptr, nullptr, 0); return; }
		    const uint8_t st = m[0], ch = st & 0x0F, d1 = m.size() > 1 ? m[1] : 0, d2 = m.size() > 2 ? m[2] : 0;
		    switch (st & 0xF0)
		    {
		    case 0x80: fluid_synth_noteoff(syn, ch, d1); break;
		    case 0x90: fluid_synth_noteon(syn, ch, d1, d2); break;
		    case 0xA0: fluid_synth_key_pressure(syn, ch, d1, d2); break;
		    case 0xB0: fluid_synth_cc(syn, ch, d1, d2); break;
		    case 0xC0: fluid_synth_program_change(syn, ch, d1); break;
		    case 0xD0: fluid_synth_channel_pressure(syn, ch, d1); break;
		    case 0xE0: fluid_synth_pitch_bend(syn, ch, (d2 << 7) | d1); break;
		    }
	    });
	fclose(out);
	printf("fluid done\n");
	return 0;
}
#endif

#ifdef WITH_MUNT
static int MainMunt(int argc, char** argv)
{
	if (argc < 6) return 1;
	using namespace MT32Emu;
	FileStream ctrlF, pcmF;
	if (!ctrlF.open(argv[2]) || !pcmF.open(argv[3])) { fprintf(stderr, "rom open failed\n"); return 3; }
	const ROMImage* ctrl = ROMImage::makeROMImage(&ctrlF);
	const ROMImage* pcm  = ROMImage::makeROMImage(&pcmF);
	Synth synth;
	if (!synth.open(*ctrl, *pcm)) { fprintf(stderr, "synth open failed (ROMs not recognised?)\n"); return 3; }
	// mt32-pi defaults (config.def): gain 1.0, reverb gain 1.0, resampler GOOD to 48 kHz
	synth.setOutputGain(1.0f);
	synth.setReverbOutputGain(1.0f);
	SampleRateConverter src(synth, SampleRate, SamplerateConversionQuality_GOOD);

	uint32_t seq = 0;
	std::vector<Event> ev;
	double t0 = 0;
	double until = -1;
	int solo = 0;
	for (int a = 6; a < argc; ++a)
	{
		if (!strncmp(argv[a], "until=", 6)) { until = atof(argv[a] + 6); continue; }
		if (!strncmp(argv[a], "solo=", 5)) { solo = atoi(argv[a] + 5); continue; }
		auto s = EndsWith(argv[a], ".mid") ? ParseSMF(ReadFile(argv[a]), seq) : ParseSyx(ReadFile(argv[a]), seq);
		for (auto& e : s) { e.t += t0; ev.push_back(e); }
		t0 = (ev.empty() ? t0 : ev.back().t) + 0.5;
	}
	auto mid = ParseSMF(ReadFile(argv[4]), seq);
	for (auto& e : mid)
	{
		// until=S: stop S seconds into the MIDI file (everything before it is still played)
		if (until >= 0 && e.t > until) break;
		// solo=N: drop note-ons on all MIDI channels but N (1-16); SysEx and controllers stay
		if (solo && (e.data[0] & 0xF0) == 0x90 && (e.data[0] & 0x0F) != solo - 1) continue;
		e.t += t0;
		ev.push_back(e);
	}
	fprintf(stderr, "MIDI file starts %.3f s into the render\n", t0);

	FILE* out = fopen(argv[5], "wb");
	Run(ev, 3.0, out,
	    [&](float* b, unsigned n) { src.getOutputSamples(b, n); },
	    [&](const std::vector<uint8_t>& m) {
		    if (m[0] == 0xF0) { synth.playSysex(m.data(), Bit32u(m.size())); return; }
		    uint32_t msg = 0;
		    for (size_t i = 0; i < m.size(); ++i) msg |= uint32_t(m[i]) << (8 * i);
		    synth.playMsg(msg);
	    });
	fclose(out);
	synth.close();
	printf("munt done\n");
	return 0;
}
#endif

int main(int argc, char** argv)
{
	if (argc < 2) return 1;
#ifdef WITH_FLUID
	if (!strcmp(argv[1], "fluid")) return MainFluid(argc, argv);
#endif
#ifdef WITH_MUNT
	if (!strcmp(argv[1], "munt")) return MainMunt(argc, argv);
#endif
	fprintf(stderr, "unknown mode\n");
	return 1;
}
