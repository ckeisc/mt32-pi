#
# Makefile
#

include Config.mk

.DEFAULT_GOAL=all
.PHONY: submodules circle-stdlib mt32emu fluidsynth emusc nuked all clean veryclean

#
# Functions to apply/reverse patches only if not completely applied/reversed already
#
APPLY_PATCH=sh -c '																\
	if patch --dry-run --reverse --force --strip 1 --directory $$1 < $$2 >/dev/null 2>&1; then						\
		echo "Patch $$2 already applied; skipping";											\
	else																	\
		echo "Applying patch $$2 to directory $$1...";											\
		patch --forward --strip 1 --no-backup-if-mismatch -r - --directory $$1 < $$2 || (echo "Patch $$2 failed" >&2 && exit 1);	\
	fi' APPLY_PATCH

REVERSE_PATCH=sh -c '																\
	if patch --dry-run --forward --force --strip 1 --directory $$1 < $$2 >/dev/null 2>&1; then						\
		echo "Patch $$2 already reversed; skipping";											\
	else																	\
		echo "Reversing patch $$2 in directory $$1...";											\
		patch --reverse --strip 1 --no-backup-if-mismatch -r - --directory $$1 < $$2 || (echo "Patch $$2 failed" >&2 && exit 1);	\
	fi' REVERSE_PATCH

#
# Get submodules
#
submodules:
	@git submodule update --init --depth 1
	@git -C external/circle-stdlib submodule update --init --depth 1 libs/circle libs/circle-newlib
	@git -C external/circle-stdlib/libs/circle submodule update --init --depth 1 addon/wlan/hostap
	@git -C external/fluidsynth submodule update --init --depth 1 gcem

#
# Configure circle-stdlib
#
$(CIRCLE_STDLIB_CONFIG) $(CIRCLE_CONFIG)&:
	@echo "Configuring for Raspberry Pi $(RASPBERRYPI) ($(BITS) bit)"
	$(CIRCLESTDLIBHOME)/configure --raspberrypi=$(RASPBERRYPI) --prefix=$(PREFIX)

# Leave out USB drivers we don't use (replaces the old minimal-usb-drivers patch).
# These guards are in Circle's lib/usb/usbdevicefactory.cpp (since Step 48) but
# aren't documented upstream. USB MIDI, mass storage, serial and network stay in.
	@echo "DEFINE += -DEXCLUDE_USB_KEYB -DEXCLUDE_USB_MOUSE -DEXCLUDE_USB_GAMEPAD" >> $(CIRCLE_CONFIG)
	@echo "DEFINE += -DEXCLUDE_USB_PRINTER -DEXCLUDE_USB_BLUETOOTH -DEXCLUDE_USB_TOUCHSCREEN" >> $(CIRCLE_CONFIG)

# Enable multi-core
	@echo "DEFINE += -DARM_ALLOW_MULTI_CORE" >> $(CIRCLE_CONFIG)

# Disable delay loop calibration (boot speed improvement)
	@echo "DEFINE += -DNO_CALIBRATE_DELAY" >> $(CIRCLE_CONFIG)

# Improve I/O throughput
	@echo "DEFINE += -DNO_BUSY_WAIT" >> $(CIRCLE_CONFIG)

# The Nuked-SC55 backend pushes the kernel image past Circle's default
# 2 MB KERNEL_MAX_SIZE, which would leave the kernel stack inside .rodata.
# 4 MB is kept deliberately (2026-10-23) so the image has room to grow.
	@echo "DEFINE += -DKERNEL_MAX_SIZE=0x400000" >> $(CIRCLE_CONFIG)

# Enable PWM audio output on GPIO 12/13 for the Pi Zero 2 W
	@echo "DEFINE += -DUSE_PWM_AUDIO_ON_ZERO" >> $(CIRCLE_CONFIG)

#
# Build circle-stdlib
#
circle-stdlib: $(CIRCLESTDLIBHOME)/.done

$(CIRCLESTDLIBHOME)/.done: $(CIRCLE_STDLIB_CONFIG)
	@$(MAKE) -C $(CIRCLESTDLIBHOME)
	touch $@

#
# Build mt32emu
#
mt32emu: $(MT32EMUBUILDDIR)/.done

$(MT32EMUBUILDDIR)/.done: $(CIRCLESTDLIBHOME)/.done
	@CFLAGS="$(CFLAGS_EXTERNAL)" \
	CXXFLAGS="$(CFLAGS_EXTERNAL)" \
	cmake -B $(MT32EMUBUILDDIR) \
		 $(CMAKE_TOOLCHAIN_FLAGS) \
		 -DCMAKE_CXX_FLAGS_RELEASE="-Ofast" \
		 -DCMAKE_BUILD_TYPE=Release \
		 -Dlibmt32emu_C_INTERFACE=FALSE \
		 -Dlibmt32emu_SHARED=FALSE \
		 -DBUILD_TESTING=OFF \
		 $(MT32EMUHOME) \
		 >/dev/null
	@cmake --build $(MT32EMUBUILDDIR)
	@touch $@

#
# Build FluidSynth
#
fluidsynth: $(FLUIDSYNTHBUILDDIR)/.done

$(FLUIDSYNTHBUILDDIR)/.done: $(CIRCLESTDLIBHOME)/.done
	@${APPLY_PATCH} $(FLUIDSYNTHHOME) patches/fluidsynth-2.6.1-circle.patch

	@CFLAGS="$(CFLAGS_EXTERNAL)" \
	CXXFLAGS="$(CFLAGS_EXTERNAL)" \
	cmake -B $(FLUIDSYNTHBUILDDIR) \
		 $(CMAKE_TOOLCHAIN_FLAGS) \
		 -DCMAKE_C_FLAGS_RELEASE="-Ofast -fopenmp-simd" \
		 -DCMAKE_CXX_FLAGS_RELEASE="-Ofast -fopenmp-simd" \
		 -Dosal=embedded \
		 -DCMAKE_BUILD_TYPE=Release \
		 -DBUILD_SHARED_LIBS=OFF \
		 -Denable-aufile=OFF \
		 -Denable-dbus=OFF \
		 -Denable-dsound=OFF \
		 -Denable-floats=ON \
		 -Denable-ipv6=OFF \
		 -Denable-jack=OFF \
		 -Denable-ladspa=OFF \
		 -Denable-libsndfile=OFF \
		 -Denable-midishare=OFF \
		 -Denable-native-dls=OFF \
		 -Denable-network=OFF \
		 -Denable-oboe=OFF \
		 -Denable-openmp=OFF \
		 -Denable-opensles=OFF \
		 -Denable-oss=OFF \
		 -Denable-pipewire=OFF \
		 -Denable-pulseaudio=OFF \
		 -Denable-readline=OFF \
		 -Denable-sdl3=OFF \
		 -Denable-signalsmith=OFF \
		 -Denable-threads=OFF \
		 -Denable-wasapi=OFF \
		 -Denable-waveout=OFF \
		 -Denable-winmidi=OFF \
		 $(FLUIDSYNTHHOME) \
		 >/dev/null
	@cmake --build $(FLUIDSYNTHBUILDDIR) --target libfluidsynth
	@touch $@


#
# Build libEmuSC
#
emusc: $(EMUSCBUILDDIR)/.done

$(EMUSCBUILDDIR)/.done: $(CIRCLESTDLIBHOME)/.done
	@${APPLY_PATCH} $(EMUSCHOME)/.. patches/emusc-0.7.0-circle.patch

	@CFLAGS="$(CFLAGS_EXTERNAL)" \
	CXXFLAGS="$(CFLAGS_EXTERNAL)" \
	cmake -B $(EMUSCBUILDDIR) \
		 $(CMAKE_TOOLCHAIN_FLAGS) \
		 -DCMAKE_CXX_FLAGS_RELEASE="-Ofast" \
		 -DCMAKE_CXX_FLAGS="-include $(CURDIR)/include/emuscirclecompat.h" \
		 -DCMAKE_BUILD_TYPE=Release \
		 -DBUILD_SHARED_LIBS=OFF \
		 $(EMUSCHOME) \
		 >/dev/null
	@cmake --build $(EMUSCBUILDDIR)

# Stage the public headers as <emusc/*.h> (libEmuSC's own install rules
# omit some headers that synth.h includes, so copy them all)
	@mkdir -p $(EMUSCBUILDDIR)/include/emusc
	@cp $(EMUSCHOME)/src/*.h $(EMUSCBUILDDIR)/include/emusc/
	@touch $@

#
# Build Nuked-SC55
#
nuked: $(NUKEDBUILDDIR)/.done

$(NUKEDBUILDDIR)/.done: $(CIRCLESTDLIBHOME)/.done
	@${APPLY_PATCH} $(NUKEDHOME) patches/nuked-sc55-0.8.0-circle.patch

	@CFLAGS="$(CFLAGS_EXTERNAL)" \
	CXXFLAGS="$(CFLAGS_EXTERNAL)" \
	cmake -B $(NUKEDBUILDDIR) \
		 $(CMAKE_TOOLCHAIN_FLAGS) \
		 -DCMAKE_CXX_FLAGS_RELEASE="-Ofast" \
		 -DCMAKE_CXX_FLAGS="-include $(CURDIR)/include/nukedcirclecompat.h" \
		 -DCMAKE_BUILD_TYPE=Release \
		 -DBUILD_SHARED_LIBS=OFF \
		 $(NUKEDHOME) \
		 >/dev/null
	@cmake --build $(NUKEDBUILDDIR) --target nuked-sc55-backend
	@touch $@

#
# Build kernel itself
#
all: circle-stdlib mt32emu fluidsynth emusc nuked
	@$(MAKE) -f Kernel.mk $(KERNEL).img $(KERNEL).hex

#
# Clean kernel only
#
clean:
# CHECK_DEPS=0: don't generate dependency files just to delete them
	@$(MAKE) -f Kernel.mk clean CHECK_DEPS=0

#
# Clean kernel and all dependencies
#
mrproper: clean
# Reverse patches
	@${REVERSE_PATCH} $(FLUIDSYNTHHOME) patches/fluidsynth-2.6.1-circle.patch

# Clean circle-stdlib
	@if [ -f $(CIRCLE_STDLIB_CONFIG) ]; then $(MAKE) -C $(CIRCLESTDLIBHOME) mrproper; fi
	@$(RM) $(CIRCLESTDLIBHOME)/.done

# Clean mt32emu
	@$(RM) -r $(MT32EMUBUILDDIR)

# Clean FluidSynth
	@$(RM) -r $(FLUIDSYNTHBUILDDIR)

# Clean libEmuSC
	@${REVERSE_PATCH} $(EMUSCHOME)/.. patches/emusc-0.7.0-circle.patch
	@$(RM) -r $(EMUSCBUILDDIR)

# Clean Nuked-SC55
	@${REVERSE_PATCH} $(NUKEDHOME) patches/nuked-sc55-0.8.0-circle.patch
	@$(RM) -r $(NUKEDBUILDDIR)
