/*
 * partial_report: play MIDI through libmt32emu (2.8 or later) and report what
 * happens to the notes.
 *
 * Usage:
 *   partial_report --rom-dir DIR --machine-id ID [--sysex FILE]... [--channel N] FILE.mid
 *
 *   --machine-id ID  a specific machine, e.g. mt32_1_07, mt32_2_04, cm32l_1_02
 *   --sysex FILE     send this first: a raw .syx file (F0 ... F7 messages) or a
 *                    MIDI file; repeatable, sent in order
 *   --channel N      also print a timeline for MIDI channel N (1-16): notes
 *                    received vs. notes playing on its part, and the part's
 *                    output level (volume)
 *
 * Reports every Note On the partial allocator ignores for lack of partials, and
 * every one that makes it cut a playing note short, with a per-channel summary.
 *
 * Events go through libmt32emu's timestamped MIDI queue as in mt32-pi, so the
 * library applies its MIDI cable delay and replays notes that had to wait for a
 * voice to be cut. Only one Note On is in the queue at a time, so every report
 * belongs to exactly that note.
 *
 * Built by run.sh --report. GPLv3 like mt32-pi; uses libmt32emu (LGPL) and
 * libsmf (BSD) from Munt.
 */

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <dirent.h>

extern "C" {
#include "smf.h"
}

#define MT32EMU_API_TYPE 3
#include <mt32emu/mt32emu.h>

namespace {

using MT32Emu::Bit8u;
using MT32Emu::Bit32s;
using MT32Emu::Bit32u;

// libmt32emu internal memory addresses (MT32EMU_MEMADDR of the SysEx address)
const Bit32u ADDR_PATCH_TEMP = 0x0C000;      // 03 00 00, 16 bytes per part
const Bit32u ADDR_TIMBRE_TEMP = 0x10000;     // 04 00 00, 246 bytes per part
const Bit32u ADDR_PARTIAL_RESERVE = 0x40004; // 10 00 04, 9 bytes
const Bit32u ADDR_MIDI_CHANNELS = 0x4000D;   // 10 00 0D, 9 bytes
const Bit32u TIMBRE_PARAM_SIZE = 246;

// libmt32emu's MIDI cable model: internal samples (32 kHz) per byte at 31250 baud
const double MIDI_SAMPLES_PER_BYTE = 32000.0 / 31250.0 * 8.0;

// SysEx (7-bit) addresses of Timbre Memory: 64 timbres from 08 00 00. Writes
// to 09 00 00 - 0B 7F 7F reached other timbre banks before Munt 2.8.2 (#146).
const Bit32u SYSEX_TIMBRE_MEMORY_END = 0x09 << 14;
const Bit32u SYSEX_OLD_TIMBRE_REGION_END = 0x0C << 14;

struct Note {
	double seconds;
	int channel, key, velocity;
};

struct Counts {
	unsigned ignored = 0, cutShort = 0;
};

struct Window {
	unsigned noteOns = 0, maxPlaying = 0, ignored = 0, cutShort = 0;
	int level = -1; // part output level (volume) at end of window
};

class Reporter : public MT32Emu::IReportHandlerV2 {
public:
	MT32Emu::Service *service = nullptr;
	const Note *current = nullptr;
	bool notePlayed = false;
	std::map<int, Counts> perChannel;
	unsigned printed = 0;
	int watchChannel = -1;
	double windowSeconds = 0.5;
	std::map<long, Window> windows;
	std::vector<std::string> programChanges;
	double songOffset = 0, nowSeconds = 0;
	bool inSetup = true;

	void printDebug(const char *, va_list) override {}
	void onErrorControlROM() override { fprintf(stderr, "Control ROM error\n"); }
	void onErrorPCMROM() override { fprintf(stderr, "PCM ROM error\n"); }
	void showLCDMessage(const char *) override {}
	bool onMIDIQueueOverflow() override { fprintf(stderr, "MIDI queue overflow; event dropped\n"); return false; }
	void onMIDISystemRealtime(Bit8u) override {}
	void onDeviceReset() override {}
	void onDeviceReconfig() override {}
	void onNewReverbMode(Bit8u) override {}
	void onNewReverbTime(Bit8u) override {}
	void onNewReverbLevel(Bit8u) override {}
	void onPolyStateChanged(Bit8u) override {}
	void onLCDStateUpdated() override {}
	void onMidiMessageLEDStateUpdated(bool) override {}

	void onProgramChanged(Bit8u partNum, const char *group, const char *patch) override {
		if (watchChannel < 0 || partNum > 7 || !partListens(partNum, watchChannel)) return;
		char line[128];
		if (inSetup) snprintf(line, sizeof line, "     setup  part %d program change: %s %s", partNum + 1,
			group ? group : "", patch ? patch : "");
		else snprintf(line, sizeof line, "  %6.1f s  part %d program change: %s %s", nowSeconds - songOffset, partNum + 1,
			group ? group : "", patch ? patch : "");
		programChanges.push_back(line);
	}

	// Fires at the end of every short message the synth plays. A Note On that had
	// to wait for a voice to be cut fires once for the attempt, then again when
	// it's replayed.
	void onMIDIMessagePlayed() override {
		if (cutPending) cutPending = false;
		else notePlayed = true;
	}

	void onNoteOnIgnored(Bit32u needed, Bit32u free) override {
		if (!current) return;
		perChannel[current->channel].ignored++;
		if (current->channel == watchChannel) window(current->seconds).ignored++;
		report("IGNORED  ", needed, free);
	}

	void onPlayingPolySilenced(Bit32u needed, Bit32u free) override {
		cutPending = true;
		if (!current) return;
		perChannel[current->channel].cutShort++;
		if (current->channel == watchChannel) window(current->seconds).cutShort++;
		report("CUT SHORT", needed, free);
	}

	Window &window(double seconds) { return windows[long(seconds / windowSeconds)]; }

	bool partListens(int part, int channel) {
		Bit8u channels[9];
		service->readMemory(ADDR_MIDI_CHANNELS, 9, channels);
		return channels[part] == channel;
	}

private:
	bool cutPending = false;

	void report(const char *what, Bit32u needed, Bit32u free) {
		if (++printed > 300) {
			if (printed == 301) printf("  ... (further events counted in the summary only)\n");
			return;
		}
		printf("%9.3f s  %s ch %2d key %3d vel %3d  needed %u, free %u  | notes per part:",
			current->seconds, what, current->channel + 1, current->key, current->velocity, needed, free);
		for (Bit8u part = 0; part < 9; part++) {
			Bit8u keys[256], velocities[256];
			printf(" %u", service->getPlayingNotes(part, keys, velocities));
		}
		printf("\n");
	}
};

bool loadROMs(MT32Emu::Service &service, const char *romDir, const char *machineID) {
	DIR *dir = opendir(romDir);
	if (!dir) {
		fprintf(stderr, "Can't open ROM directory %s\n", romDir);
		return false;
	}
	while (dirent *entry = readdir(dir)) {
		if (entry->d_name[0] == '.') continue;
		std::string path = std::string(romDir) + "/" + entry->d_name;
		service.addMachineROMFile(machineID, path.c_str());
	}
	closedir(dir);
	mt32emu_rom_info info;
	service.getROMInfo(&info);
	if (!info.control_rom_id || !info.pcm_rom_id) {
		fprintf(stderr, "No complete ROM set for machine %s in %s\n", machineID, romDir);
		return false;
	}
	printf("Control ROM: %s (%s)\nPCM ROM:     %s\n", info.control_rom_id, info.control_rom_description, info.pcm_rom_id);
	return true;
}

class Player {
	MT32Emu::Service &service;
	Reporter &reporter;
	std::vector<MT32Emu::Bit16s> buffer;

public:
	const double rate;

	Player(MT32Emu::Service &useService, Reporter &useReporter)
		: service(useService), reporter(useReporter), buffer(2 * 512), rate(useService.getActualStereoOutputSamplerate()) {}

	double now() const { return service.getInternalRenderedSampleCount() / rate; }

	void renderUntil(Bit32u target) {
		while (Bit32s(target - service.getInternalRenderedSampleCount()) > 0) {
			Bit32u frames = target - service.getInternalRenderedSampleCount();
			if (frames > 512) frames = 512;
			service.renderBit16s(buffer.data(), frames);
			sample();
		}
	}

	void drainQueue() { drain(); }

	void renderSeconds(double seconds) { renderUntil(service.getInternalRenderedSampleCount() + Bit32u(seconds * rate)); }

	void sysex(const Bit8u *data, Bit32u len, Bit32u timestamp) {
		checkTimbreWrite(data, len);
		renderUntil(timestamp);
		drain();
		service.playSysexAt(data, len, timestamp);
		renderUntil(service.getInternalRenderedSampleCount() + 1);
	}

	// Plays a short message; returns after a Note On has really been played.
	void shortMessage(Bit32u msg, Bit32u timestamp, const Note *note) {
		renderUntil(timestamp);
		if (!note) {
			queueShort(msg, timestamp);
			return;
		}
		// Let everything queued earlier play first, so only this note is in flight
		drain();
		reporter.current = note;
		reporter.notePlayed = false;
		if (note->channel == reporter.watchChannel) reporter.window(note->seconds).noteOns++;
		queueShort(msg, timestamp);
		if (!anyPartListens(note->channel)) {
			reporter.current = nullptr;
			return;
		}
		const Bit32u giveUp = service.getInternalRenderedSampleCount() + Bit32u(2 * rate);
		while (!reporter.notePlayed && Bit32s(giveUp - service.getInternalRenderedSampleCount()) > 0) {
			service.renderBit16s(buffer.data(), 1);
			sample();
		}
		if (!reporter.notePlayed) stuckNotes++;
		reporter.current = nullptr;
	}

	void playSMF(smf_t *smf, double offsetSeconds) {
		smf_rewind(smf);
		while (smf_event_t *event = smf_get_next_event(smf)) {
			if (smf_event_is_metadata(event)) continue;
			const Bit8u *buf = event->midi_buffer;
			const int len = event->midi_buffer_length;
			if (len < 1) continue;
			const double seconds = event->time_seconds + offsetSeconds;
			const Bit32u ts = Bit32u(seconds * rate);
			if (buf[0] == 0xF0) {
				sysex(buf, Bit32u(len), ts);
				continue;
			}
			if (len > 3 || buf[0] >= 0xF0) continue;
			Bit32u msg = 0;
			for (int i = 0; i < len; i++) msg |= Bit32u(buf[i]) << (8 * i);
			if ((buf[0] & 0xF0) == 0x90 && len == 3 && buf[2] > 0) {
				Note note {event->time_seconds, buf[0] & 0x0F, buf[1], buf[2]};
				shortMessage(msg, ts, &note);
			} else {
				shortMessage(msg, ts, nullptr);
			}
			lastEventSeconds = seconds;
		}
	}

	void playSysexFile(const std::vector<Bit8u> &data) {
		size_t start = 0;
		bool in = false;
		unsigned count = 0;
		for (size_t i = 0; i < data.size(); i++) {
			if (data[i] == 0xF0) {
				start = i;
				in = true;
			} else if (data[i] == 0xF7 && in) {
				sysex(&data[start], Bit32u(i - start + 1), service.getInternalRenderedSampleCount());
				renderSeconds(0.02);
				in = false;
				count++;
			}
		}
		printf("  %u SysEx messages sent\n", count);
	}

	void printSystemState(const char *label) {
		Bit8u reserve[9], channels[9];
		service.readMemory(ADDR_PARTIAL_RESERVE, 9, reserve);
		service.readMemory(ADDR_MIDI_CHANNELS, 9, channels);
		printf("%s partial reserve (parts 1-8, rhythm):", label);
		for (int i = 0; i < 9; i++) printf(" %u", reserve[i]);
		printf("  | MIDI channels:");
		for (int i = 0; i < 9; i++) printf(" %u", channels[i] + 1);
		printf("\n");
	}

	void printPartSetup() {
		Bit8u channels[9];
		service.readMemory(ADDR_MIDI_CHANNELS, 9, channels);
		printf("Parts:\n");
		for (int part = 0; part < 8; part++) {
			Bit8u patch[16], name[11] = {0};
			service.readMemory(ADDR_PATCH_TEMP + part * 16, 16, patch);
			service.readMemory(ADDR_TIMBRE_TEMP + part * TIMBRE_PARAM_SIZE, 10, name);
			for (int i = 0; i < 10; i++) if (name[i] < 0x20 || name[i] > 0x7E) name[i] = '.';
			static const char groups[] = "ABMR";
			printf("  part %d (ch %2d): timbre %c%02u \"%s\"  output level %3u  pan %2u\n", part + 1, channels[part] + 1,
				groups[patch[0] & 3], patch[1] + 1, name, patch[8], patch[9]);
		}
	}

	bool anyPartListens(int channel) {
		Bit8u channels[9];
		service.readMemory(ADDR_MIDI_CHANNELS, 9, channels);
		for (int i = 0; i < 9; i++) if (channels[i] == channel) return true;
		return false;
	}

	unsigned stuckNotes = 0, timbreWritesPastMemory = 0;
	double lastEventSeconds = 0;

private:
	Bit32u lastDue = 0; // when the last queued short message is due, as libmt32emu computes it

	void queueShort(Bit32u msg, Bit32u timestamp) {
		const Bit32u len = ((msg & 0xE0) == 0xC0) ? 2 : 3;
		Bit32u due = timestamp;
		if (Bit32s(due - lastDue) < 0) due = lastDue;
		lastDue = due + Bit32u(double(len) * MIDI_SAMPLES_PER_BYTE);
		service.playMsgAt(msg, timestamp);
	}

	void drain() { renderUntil(lastDue + 1); }

	void sample() {
		reporter.nowSeconds = now();
		if (reporter.watchChannel < 0) return;
		Bit8u channels[9];
		service.readMemory(ADDR_MIDI_CHANNELS, 9, channels);
		unsigned playing = 0;
		int level = -1;
		for (Bit8u part = 0; part < 9; part++) {
			if (channels[part] != reporter.watchChannel) continue;
			Bit8u keys[256], velocities[256];
			playing += service.getPlayingNotes(part, keys, velocities);
			if (part < 8 && level < 0) {
				Bit8u patch[16];
				service.readMemory(ADDR_PATCH_TEMP + part * 16, 16, patch);
				level = patch[8];
			}
		}
		const double songSeconds = now() - reporter.songOffset;
		if (songSeconds < 0) return;
		Window &w = reporter.window(songSeconds);
		if (playing > w.maxPlaying) w.maxPlaying = playing;
		w.level = level;
	}

	void checkTimbreWrite(const Bit8u *data, Bit32u len) {
		// F0 41 <dev> 16 12 <a1> <a2> <a3> <data...> <sum> F7
		if (len < 10 || data[1] != 0x41 || data[3] != 0x16 || data[4] != 0x12 || data[2] > 0x10) return;
		const Bit32u start = (Bit32u(data[5]) << 14) | (Bit32u(data[6]) << 7) | data[7];
		const Bit32u end = start + (len - 10);
		if (data[2] == 0x10 && end > SYSEX_TIMBRE_MEMORY_END && start < SYSEX_OLD_TIMBRE_REGION_END) {
			timbreWritesPastMemory++;
		}
	}
};

bool readFile(const char *path, std::vector<Bit8u> &out) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return false;
	out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	return true;
}

} // namespace

int main(int argc, char **argv) {
	const char *romDir = nullptr, *machineID = nullptr, *midiFile = nullptr;
	std::vector<const char *> sysexFiles;
	int channel = -1;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--rom-dir") && i + 1 < argc) romDir = argv[++i];
		else if (!strcmp(argv[i], "--machine-id") && i + 1 < argc) machineID = argv[++i];
		else if (!strcmp(argv[i], "--sysex") && i + 1 < argc) sysexFiles.push_back(argv[++i]);
		else if (!strcmp(argv[i], "--channel") && i + 1 < argc) channel = atoi(argv[++i]) - 1;
		else midiFile = argv[i];
	}
	if (!romDir || !machineID || !midiFile || (channel != -1 && (channel < 0 || channel > 15))) {
		fprintf(stderr, "Usage: %s --rom-dir DIR --machine-id ID [--sysex FILE]... [--channel 1-16] FILE.mid\n", argv[0]);
		return 2;
	}

	Reporter reporter;
	reporter.watchChannel = channel;
	MT32Emu::Service service;
	reporter.service = &service;
	service.createContext(reporter);
	if (!loadROMs(service, romDir, machineID)) return 1;
	service.setStereoOutputSampleRate(0);
	if (service.openSynth() != MT32EMU_RC_OK) {
		fprintf(stderr, "Failed to open synth\n");
		return 1;
	}
	Player player(service, reporter);
	player.renderSeconds(0.1);

	for (const char *path : sysexFiles) {
		std::vector<Bit8u> data;
		if (!readFile(path, data) || data.empty()) {
			fprintf(stderr, "Can't read %s\n", path);
			return 1;
		}
		printf("Setup: %s\n", path);
		if (data.size() >= 4 && !memcmp(data.data(), "MThd", 4)) {
			smf_t *smf = smf_load_from_memory(data.data(), int(data.size()));
			if (!smf) {
				fprintf(stderr, "Failed to load %s\n", path);
				return 1;
			}
			player.playSMF(smf, player.now());
			smf_delete(smf);
		} else {
			player.playSysexFile(data);
		}
		player.renderSeconds(0.5);
	}
	if (player.timbreWritesPastMemory) {
		printf("  NOTE: %u SysEx write(s) go past Timbre Memory (08 00 00-08 7F 7F); Munt 2.8.2+ ignores these (#146)\n",
			player.timbreWritesPastMemory);
	}

	smf_t *smf = smf_load(midiFile);
	if (!smf) {
		fprintf(stderr, "Failed to load %s\n", midiFile);
		return 1;
	}
	printf("File: %s (%.1f s)\n", midiFile, smf_get_length_seconds(smf));
	player.drainQueue();
	reporter.songOffset = player.now();
	reporter.inSetup = false;
	reporter.windows.clear();
	player.printSystemState("At start:");
	printf("\n");
	player.playSMF(smf, reporter.songOffset);
	player.renderUntil(Bit32u((player.lastEventSeconds + 2.0) * player.rate));

	printf("\n");
	player.printSystemState("At end:  ");
	player.printPartSetup();

	printf("\nSummary (MIDI channel: notes ignored / playing notes cut short):\n");
	if (reporter.perChannel.empty()) printf("  none - every note found partials\n");
	for (auto &entry : reporter.perChannel) {
		printf("  ch %2d: %u ignored, %u cut short\n", entry.first + 1, entry.second.ignored, entry.second.cutShort);
	}
	if (player.stuckNotes) printf("  %u Note On(s) never confirmed as played within 2 s\n", player.stuckNotes);

	if (channel >= 0) {
		printf("\nTimeline for MIDI channel %d (%.1f s windows; 'playing' = most notes playing at once, "
			"'level' = part volume 0-100):\n",
			channel + 1, reporter.windowSeconds);
		for (const std::string &line : reporter.programChanges) printf("%s\n", line.c_str());
		printf("      time  notes in  playing  level  ignored  cut short\n");
		unsigned lines = 0;
		for (auto &entry : reporter.windows) {
			const Window &w = entry.second;
			if (!w.noteOns && !w.maxPlaying) continue;
			if (++lines > 600) {
				printf("  ...\n");
				break;
			}
			const char *flag = "";
			if (w.noteOns && !w.maxPlaying) flag = "   <-- no notes playing";
			else if (w.maxPlaying && w.level == 0) flag = "   <-- volume 0";
			printf("  %6.1f s  %8u  %7u  %5d  %7u  %9u%s\n", entry.first * reporter.windowSeconds, w.noteOns,
				w.maxPlaying, w.level, w.ignored, w.cutShort, flag);
		}
	}

	smf_delete(smf);
	service.closeSynth();
	service.freeContext();
	return 0;
}
