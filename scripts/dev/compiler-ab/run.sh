#!/bin/bash
#
# Compiler A/B: build libmt32emu and libfluidsynth exactly as the pim-32
# Makefile does for BOARD=pi3-64, once with each of two Arm GNU toolchains,
# link each into a semihosted renderer, render under qemu-system-aarch64 and
# compare the renders. See README.md.
#
set -euo pipefail

usage() {
	cat <<EOF
Usage: $0 [options]
  --old VER         first toolchain version (default 11.3.rel1)
  --new VER         second toolchain version (default 15.2.rel1)
  --toolchains DIR  where arm-gnu-toolchain-VER-x86_64-aarch64-none-elf live (default \$HOME)
  --opt FLAGS       optimisation flags for both libraries (default: the Makefile's -Ofast)
  --sf2 FILE        SoundFont for the FluidSynth render (skipped if not given)
  --midi FILE       MIDI file to render (default: generated GM test for FluidSynth)
  --control FILE    MT-32 control ROM   } both needed for the Munt render
  --pcm FILE        MT-32 PCM ROM       }
  --sysex FILE      setup sent to Munt before --midi (.syx or .mid); repeatable
  --until SECONDS   Munt: stop this far into --midi (earlier events still play)
  --solo CHANNEL    Munt: keep note-ons on this MIDI channel (1-16) only
  --work DIR        work directory (default ./compiler-ab-work)
EOF
	exit 1
}

OLD=11.3.rel1
NEW=15.2.rel1
TCDIR=$HOME
OPT=-Ofast
SF2=""
MIDI=""
CTRL=""
PCM=""
SYSEX=()
MUNTOPTS=()
WORK=$PWD/compiler-ab-work

while [ $# -gt 0 ]; do
	case $1 in
	--old) OLD=$2; shift 2 ;;
	--new) NEW=$2; shift 2 ;;
	--toolchains) TCDIR=$2; shift 2 ;;
	--opt) OPT=$2; shift 2 ;;
	--sf2) SF2=$(realpath "$2"); shift 2 ;;
	--midi) MIDI=$(realpath "$2"); shift 2 ;;
	--control) CTRL=$(realpath "$2"); shift 2 ;;
	--pcm) PCM=$(realpath "$2"); shift 2 ;;
	--sysex) SYSEX+=("$(realpath "$2")"); shift 2 ;;
	--until) MUNTOPTS+=("until=$2"); shift 2 ;;
	--solo) MUNTOPTS+=("solo=$2"); shift 2 ;;
	--work) WORK=$2; shift 2 ;;
	*) usage ;;
	esac
done

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../../.." && pwd)
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)

# FluidSynth needs pim-32's patch; apply it to a private copy of the source
if [ ! -d "$WORK/fluidsynth-src" ]; then
	cp -a "$SRC/external/fluidsynth" "$WORK/fluidsynth-src"
	patch -s -p1 -d "$WORK/fluidsynth-src" < "$SRC/patches/fluidsynth-2.6.1-circle.patch"
fi

build() {
	local ver=$1 out=$2
	local bin="$TCDIR/arm-gnu-toolchain-$ver-x86_64-aarch64-none-elf/bin"
	[ -x "$bin/aarch64-none-elf-gcc" ] || { echo "toolchain $ver not found in $TCDIR" >&2; exit 1; }
	local cf="-mcpu=cortex-a53 -mlittle-endian -ffunction-sections -fdata-sections"
	local tcf="$SRC/cmake/aarch64-none-elf.cmake"
	(
		export PATH="$bin:$PATH"
		if [ ! -f "$out/munt/libmt32emu.a" ]; then
			CFLAGS="$cf" CXXFLAGS="$cf" cmake -B "$out/munt" -DCMAKE_TOOLCHAIN_FILE="$tcf" \
				-DCMAKE_CXX_FLAGS_RELEASE="$OPT" -DCMAKE_BUILD_TYPE=Release \
				-Dlibmt32emu_C_INTERFACE=FALSE -Dlibmt32emu_SHARED=FALSE -DBUILD_TESTING=OFF \
				"$SRC/external/munt/mt32emu" > /dev/null
			cmake --build "$out/munt" -j"$(nproc)" > /dev/null
		fi
		if [ ! -f "$out/fluid/src/libfluidsynth.a" ]; then
			local off=""
			for o in aufile dbus dsound ipv6 jack ladspa libsndfile midishare native-dls network oboe \
				openmp opensles oss pipewire pulseaudio readline sdl3 signalsmith threads wasapi \
				waveout winmidi; do
				off="$off -Denable-$o=OFF"
			done
			# shellcheck disable=SC2086
			CFLAGS="$cf" CXXFLAGS="$cf" cmake -B "$out/fluid" -DCMAKE_TOOLCHAIN_FILE="$tcf" \
				-DCMAKE_C_FLAGS_RELEASE="$OPT -fopenmp-simd" -DCMAKE_CXX_FLAGS_RELEASE="$OPT -fopenmp-simd" \
				-Dosal=embedded -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -Denable-floats=ON $off \
				"$WORK/fluidsynth-src" > /dev/null
			cmake --build "$out/fluid" -j"$(nproc)" --target libfluidsynth > /dev/null 2>&1
		fi
		aarch64-none-elf-g++ -O2 -mcpu=cortex-a53 -DWITH_FLUID -DWITH_MUNT \
			-I "$out/munt/include" -I "$out/fluid/include" -I "$WORK/fluidsynth-src/include" \
			--specs=rdimon.specs -Wl,-Ttext-segment=0x40080000 \
			"$HERE/abrender.cpp" "$HERE/fpinit.S" \
			"$out/fluid/src/libfluidsynth.a" "$out/munt/libmt32emu.a" -lm -o "$out/abrender.elf"
	)
	echo "built $out/abrender.elf"
}

render() {
	local elf=$1
	shift
	local args="abrender"
	for a in "$@"; do args="$args,arg=$a"; done
	qemu-system-aarch64 -M virt -cpu cortex-a53 -m 2048 -nographic \
		-semihosting-config "enable=on,target=native,arg=$args" -kernel "$elf"
}

tag() { echo "$1$( [ "$OPT" = -Ofast ] || echo "${OPT// /}" )"; }
A=$WORK/$(tag "$OLD")
B=$WORK/$(tag "$NEW")
build "$OLD" "$A"
build "$NEW" "$B"

cd "$WORK"
if [ -n "$SF2" ]; then
	FMID=${MIDI:-$WORK/gmtest.mid}
	[ -f "$FMID" ] || python3 "$HERE/make_gm_test.py" "$FMID"
	cp -f "$SF2" sf.sf2
	cp -f "$FMID" fluid.mid
	for d in "$A" "$B"; do
		render "$d/abrender.elf" fluid sf.sf2 fluid.mid "$(basename "$d")-fluid.f32"
	done
	echo "== FluidSynth: $(basename "$A") vs $(basename "$B")"
	python3 "$HERE/cmp_f32.py" "$(basename "$A")-fluid.f32" "$(basename "$B")-fluid.f32"
fi

if [ -n "$CTRL" ] && [ -n "$PCM" ] && [ -n "$MIDI" ]; then
	cp -f "$CTRL" ctrl.rom
	cp -f "$PCM" pcm.rom
	cp -f "$MIDI" munt.mid
	setup=()
	i=0
	for s in "${SYSEX[@]}"; do
		ext=${s##*.}
		cp -f "$s" "setup$i.$ext"
		setup+=("setup$i.$ext")
		i=$((i + 1))
	done
	for d in "$A" "$B"; do
		render "$d/abrender.elf" munt ctrl.rom pcm.rom munt.mid "$(basename "$d")-munt.f32" \
			"${setup[@]}" "${MUNTOPTS[@]}"
	done
	echo "== Munt: $(basename "$A") vs $(basename "$B")"
	python3 "$HERE/cmp_f32.py" "$(basename "$A")-munt.f32" "$(basename "$B")-munt.f32"
fi
