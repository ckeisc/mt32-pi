#
# Build kernel
#

include Config.mk

OBJS		:=	src/config.o \
			src/control/control.o \
			src/control/mister.o \
			src/control/rotaryencoder.o \
			src/control/simplebuttons.o \
			src/control/simpleencoder.o \
			src/kernel.o \
			src/lcd/drivers/hd44780.o \
			src/lcd/drivers/hd44780fourbit.o \
			src/lcd/drivers/hd44780i2c.o \
			src/lcd/drivers/sh1106.o \
			src/lcd/drivers/ssd1306.o \
			src/lcd/ui.o \
			src/main.o \
			src/midimonitor.o \
			src/midiparser.o \
			src/mt32pi.o \
			src/net/applemidi.o \
			src/net/ftpdaemon.o \
			src/net/ftpworker.o \
			src/net/udpmidi.o \
			src/pisound.o \
			src/power.o \
			src/rommanager.o \
			src/soundfontmanager.o \
			src/synth/emuscsynth.o \
			src/synth/mt32synth.o \
			src/synth/nukedsc55synth.o \
			src/diaglog.o \
			src/synth/soundfontsynth.o \
			src/synth/mu50synth.o \
			src/synth/mu50h8.o \
			external/mu50/src/synth.o \
			external/mu50/src/swp00_meg.o \
			external/mu50/src/swp00_0277.o \
			src/zoneallocator.o

EXTRACLEAN	+=	src/*.d src/*.o \
			src/control/*.d src/control/*.o \
			src/lcd/*.d src/lcd/*.o \
			src/lcd/drivers/*.d src/lcd/drivers/*.o \
			src/net/*.d src/net/*.o \
			src/synth/*.d src/synth/*.o \
			external/mu50/src/*.d external/mu50/src/*.o

#
# inih
#
OBJS		+=	$(INIHHOME)/ini.o
INCLUDE		+=	-I $(INIHHOME)
EXTRACLEAN	+=	$(INIHHOME)/ini.d \
			$(INIHHOME)/ini.o

# libEmuSC requires C++17; the rest of the kernel is built as C++14
src/synth/emuscsynth.o: STANDARD = -std=c++17 -Wno-aligned-new

# libmu50 requires C++17 (structured bindings, std::clamp); the engine
# wrapper and the vendored lib sources are compiled as C++17, like
# EmuSC above (see external/mu50/README.md for the snapshot note)
src/synth/mu50synth.o: STANDARD = -std=c++17 -Wno-aligned-new
# The MU50 H8 CPU port is C++17, like the libmu50 wrapper above
src/synth/mu50h8.o: STANDARD = -std=c++17 -Wno-aligned-new
external/mu50/src/synth.o: STANDARD = -std=c++17 -Wno-aligned-new
external/mu50/src/swp00_0277.o: STANDARD = -std=c++17 -Wno-aligned-new
external/mu50/src/swp00_meg.o: STANDARD = -std=c++17 -Wno-aligned-new

# The Nuked-SC55 backend headers require C++20 (the backend library
# itself is built separately by the top-level Makefile)
src/synth/nukedsc55synth.o: STANDARD = -std=c++20 -Wno-aligned-new

include $(CIRCLEHOME)/Rules.mk

CFLAGS		+=	-Werror -Wextra -Wno-unused-parameter

# circle-stdlib v16 wrote these into Circle's Config.mk; v21 no longer does
STDDEF_INCPATH	?=	$(shell $(PREFIX)gcc -print-file-name=include)
CFLAGS		+=	-Wno-parentheses

CFLAGS		+=	-I "$(NEWLIBDIR)/include" \
			-I $(STDDEF_INCPATH) \
			-isystem $(CIRCLESTDLIBHOME)/include \
			-I include \
			-I .

LIBS 		:=	$(CIRCLE_STDLIB_LIBS) \
			$(CIRCLEHOME)/addon/fatfs/libfatfs.a \
			$(CIRCLEHOME)/addon/SDCard/libsdcard.a \
			$(CIRCLEHOME)/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a \
			$(CIRCLEHOME)/addon/wlan/libwlan.a \
			$(CIRCLEHOME)/lib/fs/libfs.a \
			$(CIRCLEHOME)/lib/input/libinput.a \
			$(CIRCLEHOME)/lib/libcircle.a \
			$(CIRCLEHOME)/lib/net/libnet.a \
			$(CIRCLEHOME)/lib/sched/libsched.a \
			$(CIRCLEHOME)/lib/sound/libsound.a \
			$(CIRCLEHOME)/lib/usb/libusb.a

ifeq ($(HDMI_CONSOLE), 1)
DEFINE		+=	-D HDMI_CONSOLE
endif

-include $(DEPS)

INCLUDE		+=	-I $(MT32EMUBUILDDIR)/include
EXTRALIBS	+=	$(MT32EMULIB)

INCLUDE		+=	-I $(FLUIDSYNTHBUILDDIR)/include \
			-I $(FLUIDSYNTHHOME)/include
EXTRALIBS	+=	$(FLUIDSYNTHLIB)

INCLUDE		+=	-I $(EMUSCBUILDDIR)/include
EXTRALIBS	+=	$(EMUSCLIB)

EXTRALIBS	+=	$(NUKEDLIB)

# The wrapper TU is compiled against the backend's headers in place (its
# own source tree plus the generated config.h), with the same include
# layout the backend's own build uses. Scoped to this TU (and its dep
# file) so the backend's generated config.h can never shadow the
# kernel's own headers for other translation units.
src/synth/nukedsc55synth.o: CPPFLAGS += -I $(NUKEDHOME)/src/backend -I $(NUKEDBUILDDIR)/backend
src/synth/nukedsc55synth.d: CPPFLAGS += -I $(NUKEDHOME)/src/backend -I $(NUKEDBUILDDIR)/backend

# libmu50 public header for the wrapper TU and the vendored lib sources
# (the lib's own headers live beside its sources and are found via the
# quoted-include search, but synth.hpp is under include/)
src/synth/mu50synth.o: CPPFLAGS += -I external/mu50/include
src/synth/mu50synth.d: CPPFLAGS += -I external/mu50/include
external/mu50/src/synth.o: CPPFLAGS += -I external/mu50/include
external/mu50/src/synth.d: CPPFLAGS += -I external/mu50/include
external/mu50/src/swp00_0277.o: CPPFLAGS += -I external/mu50/include
external/mu50/src/swp00_0277.d: CPPFLAGS += -I external/mu50/include
external/mu50/src/swp00_meg.o: CPPFLAGS += -I external/mu50/include
external/mu50/src/swp00_meg.d: CPPFLAGS += -I external/mu50/include

#
# Generate version string from git tag
#
VERSION=$(shell git describe --tags --dirty --always 2>/dev/null)
ifneq ($(VERSION),)
DEFINE		+=	-D MT32_PI_VERSION=\"$(VERSION)\"
endif
