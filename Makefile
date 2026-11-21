# SPDX-License-Identifier: GPL-2.0
#
# upstream/ is the Linux driver, unmodified, as a git submodule. shim/ gives it the kernel headers
# it expects, and mac/ supplies what those headers promise plus the transport.
#
#   make              the capture library and the hd60s-grab test tool
#   make check        offline self-test, no device needed
#   make plugin       the OBS Studio source plugin
#   make install-obs  copy the plugin into OBS's per-user plugin folder

ifeq ($(wildcard upstream/hd60s.h),)
$(error upstream/ is empty; run "git submodule update --init")
endif

LIBUSB_PREFIX ?= $(shell brew --prefix libusb 2>/dev/null)
OBS_APP       ?= /Applications/OBS.app
OBS_VERSION   ?= $(shell /usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" $(OBS_APP)/Contents/Info.plist 2>/dev/null)
OBS_PLUGINS   ?= $(HOME)/Library/Application Support/obs-studio/plugins

CC      = cc
CFLAGS  = -O2 -g -std=gnu11 -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter \
          -Ishim -Iupstream -Imac -I$(LIBUSB_PREFIX)/include/libusb-1.0
# libusb is linked statically so the OBS plugin carries no Homebrew dependency.
USBLIBS = $(LIBUSB_PREFIX)/lib/libusb-1.0.a \
          -framework IOKit -framework CoreFoundation -framework Security
LDFLAGS = $(USBLIBS) -framework CoreGraphics -framework ImageIO

BUILD = build

# hd60s-video.c and hd60s-audio.c are V4L2 and ALSA; mac/video.c replaces them.
UPSTREAM = hd60s-core hd60s-mstar hd60s-rev4 hd60s-modes hd60s-parse
LIBOBJS  = $(UPSTREAM:%=$(BUILD)/%.o) $(BUILD)/kshim.o $(BUILD)/video.o
LIB      = $(BUILD)/libhd60s.a

OBS_SRC = $(BUILD)/obs-studio
SIMDE   = $(BUILD)/simde
PLUGIN  = $(BUILD)/hd60s-capture.plugin

all: hd60s-grab

hd60s-grab: $(BUILD)/grab.o $(LIB)
	$(CC) -o $@ $(BUILD)/grab.o $(LIB) $(LDFLAGS)

$(LIB): $(LIBOBJS)
	rm -f $@
	ar rcs $@ $(LIBOBJS)

$(BUILD)/%.o: upstream/%.c shim/kshim.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: mac/%.c shim/kshim.h mac/hd60s-mac.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

# Parser, frame delivery and PNG output against a synthetic stream; no device.
check: $(LIB) tests/selftest.c mac/video.c mac/grab.c
	$(CC) $(CFLAGS) -o $(BUILD)/selftest tests/selftest.c \
		$(filter-out $(BUILD)/video.o,$(LIBOBJS)) $(LDFLAGS)
	$(BUILD)/selftest

# libobs headers matching the installed OBS; the app itself ships none.
$(OBS_SRC)/libobs/obs-module.h: | $(BUILD)
	rm -rf $(OBS_SRC)
	git clone -q --depth 1 --branch $(OBS_VERSION) --filter=blob:none --sparse \
		https://github.com/obsproject/obs-studio.git $(OBS_SRC)
	cd $(OBS_SRC) && git sparse-checkout set libobs

# libobs's vector headers include SIMDe on Apple silicon; it is header-only.
$(SIMDE)/simde/x86/sse2.h: | $(BUILD)
	rm -rf $(SIMDE)
	git clone -q --depth 1 --branch v0.8.2 --filter=blob:none --sparse \
		https://github.com/simd-everywhere/simde.git $(SIMDE)
	cd $(SIMDE) && git sparse-checkout set simde

plugin: $(LIB) obs/hd60s-obs.c obs/Info.plist $(OBS_SRC)/libobs/obs-module.h $(SIMDE)/simde/x86/sse2.h
	rm -rf $(PLUGIN)
	mkdir -p $(PLUGIN)/Contents/MacOS
	$(CC) $(CFLAGS) -I$(OBS_SRC)/libobs -Iobs -isystem $(SIMDE) -bundle \
		-o $(PLUGIN)/Contents/MacOS/hd60s-capture obs/hd60s-obs.c $(LIB) $(USBLIBS) \
		-F$(OBS_APP)/Contents/Frameworks -framework libobs \
		-Wl,-exported_symbol,'_obs_module_*' -Wl,-exported_symbol,_obs_current_module
	cp obs/Info.plist $(PLUGIN)/Contents/Info.plist
	codesign --force --sign - $(PLUGIN)

# The plugin's source driven without OBS; needs the device and an HDMI source.
check-obs: $(LIB) tests/obs-harness.c obs/hd60s-obs.c $(OBS_SRC)/libobs/obs-module.h $(SIMDE)/simde/x86/sse2.h
	$(CC) $(CFLAGS) -I$(OBS_SRC)/libobs -Iobs -isystem $(SIMDE) \
		-o $(BUILD)/obs-harness tests/obs-harness.c $(LIB) $(USBLIBS) \
		-F$(OBS_APP)/Contents/Frameworks -framework libobs \
		-Wl,-rpath,$(OBS_APP)/Contents/Frameworks
	$(BUILD)/obs-harness

install-obs: plugin
	mkdir -p "$(OBS_PLUGINS)"
	rm -rf "$(OBS_PLUGINS)/hd60s-capture.plugin"
	cp -R $(PLUGIN) "$(OBS_PLUGINS)/"
	@echo "installed; restart OBS and add the 'Elgato HD60 S' source"

uninstall-obs:
	rm -rf "$(OBS_PLUGINS)/hd60s-capture.plugin"

clean:
	rm -rf $(BUILD) hd60s-grab

.PHONY: all check check-obs plugin install-obs uninstall-obs clean
