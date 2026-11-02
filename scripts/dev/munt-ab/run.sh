#!/usr/bin/env bash
#
# Offline A/B test for Munt (libmt32emu) upgrades.
#
# Builds Munt's mt32emu-smf2wav renderer twice from the same renderer source,
# once against the OLD library and once against the NEW one, renders every
# MIDI file with both, and compares the results sample by sample. Because the
# renderer is identical, any difference comes from the library alone.
#
# Requires: git, cmake, a C++ compiler, pkg-config, glib 2 development files
# (Debian/Ubuntu: sudo apt-get install build-essential cmake git pkg-config libglib2.0-dev),
# python3, and your own MT-32 / CM-32L ROM files.
#
# Usage:
#   run.sh --roms DIR [--machine ID] [--midi DIR] [--sysex FILE]... [--old REF] [--new REF]
#          [--work DIR] [--solo N] [--report [--channel N]]
#
#   --roms DIR     Directory containing MT-32 and/or CM-32L ROM files (required)
#   --machine ID   mt32emu machine ID, e.g. mt32, cm32l, mt32_1_07 (default: mt32).
#                  Old-gen units (mt32_1_04 ... mt32_1_07, mt32_bluer) show the
#                  partial allocator changes; run once with each ROM set you use.
#   --midi DIR     Directory of .mid files to render (default: generated test set)
#   --sysex FILE   Send this setup file before each MIDI file, as you would to the
#                  device (raw .syx, or a .mid of SysEx). Repeatable, sent in order.
#   --old REF      Munt git ref for the old library (default: libmt32emu_2_7_0)
#   --new REF      Munt git ref for the new library (default: libmt32emu_2_8_3)
#   --work DIR     Working directory (default: ./munt-ab-work)
#   --solo N       Render only MIDI channel N (plus all SysEx) and show the loudness
#                  of every second, old vs. new, flagging seconds that went quiet
#   --report       Instead of comparing renders, list every note the NEW library
#                  refuses or cuts short for lack of partials, and each part's
#                  setup (needs a specific --machine such as mt32_1_07)
#   --channel N    With --report: timeline for MIDI channel N showing notes
#                  received vs. notes playing on its part, and its volume
#
# Render settings mirror mt32-pi: COARSE analog filter, 48 kHz output, GOOD
# resampler quality, other options at library defaults.

set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROMS=""
MACHINE="mt32"
MIDI=""
OLD_REF="libmt32emu_2_7_0"
NEW_REF="libmt32emu_2_8_3"
WORK="./munt-ab-work"
REPORT=0
CHANNEL=""
SOLO=""
SYSEX=()
MUNT_URL="https://github.com/munt/munt.git"

usage() {
	sed -n '/^# Usage:/,/^# Render settings/p' "$0" | sed 's/^# \{0,1\}//' | sed '$d' >&2
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
	--roms) ROMS="$2"; shift 2 ;;
	--machine) MACHINE="$2"; shift 2 ;;
	--midi) MIDI="$2"; shift 2 ;;
	--old) OLD_REF="$2"; shift 2 ;;
	--new) NEW_REF="$2"; shift 2 ;;
	--work) WORK="$2"; shift 2 ;;
	--report) REPORT=1; shift ;;
	--channel) CHANNEL="$2"; shift 2 ;;
	--solo) SOLO="$2"; shift 2 ;;
	--sysex)
		[ -f "$2" ] || { echo "SysEx file not found: $2" >&2; exit 2; }
		SYSEX+=("$(cd "$(dirname "$2")" && pwd)/$(basename "$2")")
		shift 2
		;;
	-h | --help) usage ;;
	*) echo "Unknown option: $1" >&2; usage ;;
	esac
done

[ -n "$ROMS" ] || { echo "--roms is required" >&2; usage; }
[ -d "$ROMS" ] || { echo "ROM directory not found: $ROMS" >&2; exit 2; }
if [ -n "$CHANNEL" ] && [ "$REPORT" != 1 ]; then
	echo "--channel only applies with --report" >&2
	exit 2
fi
if [ -n "$SOLO" ] && [ "$REPORT" = 1 ]; then
	echo "--solo doesn't apply with --report; use --channel there" >&2
	exit 2
fi

mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
ROMS=$(cd "$ROMS" && pwd)

# 1. Munt source
REPO="$WORK/munt-repo"
if [ ! -d "$REPO/.git" ]; then
	echo "== Cloning Munt"
	git clone --quiet --filter=blob:none "$MUNT_URL" "$REPO"
fi
git -C "$REPO" fetch --quiet --tags origin

# 2. Build the renderer twice: OLD renderer + OLD library, OLD renderer + NEW library
build_renderer() {
	local name="$1" lib_ref="$2"
	local src="$WORK/src-$name" build="$WORK/build-$name"
	if [ -x "$build/mt32emu_smf2wav/mt32emu-smf2wav" ] && [ -f "$build/.ref" ] &&
		[ "$(cat "$build/.ref")" = "$OLD_REF:$lib_ref" ]; then
		return
	fi
	echo "== Building renderer ($name: library $lib_ref)"
	rm -rf "$src" "$build"
	mkdir -p "$src"
	git -C "$REPO" archive "$OLD_REF" | tar -x -C "$src"
	rm -rf "$src/mt32emu"
	git -C "$REPO" archive "$lib_ref" mt32emu | tar -x -C "$src"
	cmake -S "$src" -B "$build" -DCMAKE_BUILD_TYPE=Release \
		-Dmunt_WITH_MT32EMU_QT=OFF -Dmunt_WITH_MT32EMU_SMF2WAV=ON \
		-DBUILD_TESTING=OFF >"$WORK/cmake-$name.log"
	cmake --build "$build" -j "$(nproc 2>/dev/null || echo 4)" >"$WORK/build-$name.log"
	echo "$OLD_REF:$lib_ref" >"$build/.ref"
}
build_renderer old "$OLD_REF"
build_renderer new "$NEW_REF"

# 3. MIDI files
if [ -z "$MIDI" ]; then
	MIDI="$WORK/midi"
	rm -rf "$MIDI" # drop test files from older versions of this script
	python3 "$HERE/make_test_midi.py" "$MIDI" >/dev/null
fi

# Solo mode: copies of the MIDI files with only one channel's messages
if [ -n "$SOLO" ]; then
	solo_dir="$WORK/midi-solo-ch$SOLO"
	rm -rf "$solo_dir"
	mkdir -p "$solo_dir"
	shopt -s nullglob
	for midi in "$MIDI"/*.mid "$MIDI"/*.MID; do
		python3 "$HERE/filter_midi.py" "$midi" "$solo_dir/$(basename "$midi")" "$SOLO"
	done
	shopt -u nullglob
	MIDI="$solo_dir"
fi

# Partial report mode: list notes the new library refuses or cuts short
if [ "$REPORT" = 1 ]; then
	tool="$WORK/partial_report"
	src="$WORK/src-new" build="$WORK/build-new"
	if [ ! -x "$tool" ] || [ "$HERE/partial_report.cpp" -nt "$tool" ] || [ "$build/.ref" -nt "$tool" ]; then
		echo "== Building partial_report"
		# shellcheck disable=SC2046 # pkg-config output must word-split
		g++ -std=c++11 -O2 \
			-I"$build/mt32emu/include" \
			-I"$src/mt32emu_smf2wav/libsmf/src" -I"$build/mt32emu_smf2wav/libsmf" \
			$(pkg-config --cflags glib-2.0) \
			"$HERE/partial_report.cpp" \
			"$build/mt32emu_smf2wav/libsmf/libsmf.a" "$build/mt32emu/libmt32emu.a" \
			$(pkg-config --libs glib-2.0) -lm -o "$tool"
	fi
	shopt -s nullglob
	files=("$MIDI"/*.mid "$MIDI"/*.MID)
	[ ${#files[@]} -gt 0 ] || { echo "No .mid files in $MIDI" >&2; exit 2; }
	for midi in "${files[@]}"; do
		echo
		echo "######## $(basename "$midi") ($MACHINE)"
		args=(--rom-dir "$ROMS" --machine-id "$MACHINE")
		for f in ${SYSEX[@]+"${SYSEX[@]}"}; do args+=(--sysex "$f"); done
		[ -z "$CHANNEL" ] || args+=(--channel "$CHANNEL")
		"$tool" "${args[@]}" "$midi"
	done
	exit 0
fi

# 4. Render
render() {
	local name="$1" out="$WORK/render-$MACHINE${SOLO:+-ch$SOLO}-$1"
	local tool="$WORK/build-$name/mt32emu_smf2wav/mt32emu-smf2wav"
	rm -rf "$out" # no stale renders from earlier runs
	mkdir -p "$out"
	echo "== Rendering with $name library ($("$tool" --help 2>&1 | grep -o 'Library Version [0-9.]*'))"
	shopt -s nullglob
	local files=("$MIDI"/*.mid "$MIDI"/*.MID)
	[ ${#files[@]} -gt 0 ] || { echo "No .mid files in $MIDI" >&2; exit 2; }
	for midi in "${files[@]}"; do
		local base
		base=$(basename "${midi%.*}")
		"$tool" --quiet --force --rom-dir="$ROMS" --machine-id="$MACHINE" \
			--analog-output-mode=1 --sample-rate=48000 --src-quality=2 \
			--record-max-start-silence=-1 \
			--output="$out/$base.wav" ${SYSEX[@]+"${SYSEX[@]}"} "$midi"
	done
}
render old
render new

# 5. Compare
echo "== Comparing ($MACHINE)"
levels=()
[ -z "$SOLO" ] || levels=(--levels)
python3 "$HERE/compare_wavs.py" ${levels[@]+"${levels[@]}"} "$WORK/render-$MACHINE${SOLO:+-ch$SOLO}-old" "$WORK/render-$MACHINE${SOLO:+-ch$SOLO}-new"
