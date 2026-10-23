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
			src/synth/soundfontsynth.o \
			src/zoneallocator.o

EXTRACLEAN	+=	src/*.d src/*.o \
			src/control/*.d src/control/*.o \
			src/lcd/*.d src/lcd/*.o \
			src/lcd/drivers/*.d src/lcd/drivers/*.o \
			src/net/*.d src/net/*.o \
			src/synth/*.d src/synth/*.o

#
# inih
#
OBJS		+=	$(INIHHOME)/ini.o
INCLUDE		+=	-I $(INIHHOME)
EXTRACLEAN	+=	$(INIHHOME)/ini.d \
			$(INIHHOME)/ini.o

# libEmuSC requires C++17; the rest of the kernel is built as C++14
src/synth/emuscsynth.o: STANDARD = -std=c++17 -Wno-aligned-new

# The Nuked-SC55 backend headers require C++20 (the backend library
# itself is built separately by the top-level Makefile)
src/synth/nukedsc55synth.o: STANDARD = -std=c++20 -Wno-aligned-new

include $(CIRCLEHOME)/Rules.mk

CFLAGS		+=	-Werror -Wextra -Wno-unused-parameter

CFLAGS		+=	-I "$(NEWLIBDIR)/include" \
			-I $(STDDEF_INCPATH) \
			-I $(CIRCLESTDLIBHOME)/include \
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

#
# Generate version string from git tag
#
VERSION=$(shell git describe --tags --dirty --always 2>/dev/null)
ifneq ($(VERSION),)
DEFINE		+=	-D MT32_PI_VERSION=\"$(VERSION)\"
endif
