# Cross-build the example Toadi Broker plug-in for the robot (aarch64).
#
# The Debian bullseye cross sysroot used by the rest of this workspace is
# unpacked under tools/cross-bullseye/root (see tools/fetch_toolchain.sh).
# Override CROSS on the command line if yours lives elsewhere:
#
#   make CROSS=/path/to/root
#
# The resulting .so has only libc/libstdc++ dependencies and can be copied to
# /home/toadi/plugins on the robot. It is loaded by libPlugins.so automatically
# on the next ToadiApp start.

CROSS ?= /home/demo/Downloads/toadi_work/tools/cross-bullseye/root
CXX   := $(CROSS)/usr/bin/aarch64-linux-gnu-g++-10
GCCLIB := $(CROSS)/usr/lib/gcc-cross/aarch64-linux-gnu/10

# The cross binutils in this unpacked sysroot need their host-side helper
# libraries, which live under usr/lib/x86_64-linux-gnu.
export LD_LIBRARY_PATH := $(CROSS)/usr/lib/x86_64-linux-gnu:$(LD_LIBRARY_PATH)

CXXFLAGS := --sysroot=$(CROSS) -B$(GCCLIB) -L$(GCCLIB) \
            -std=c++17 -O2 -fPIC -Wall -Wextra \
            -fvisibility=hidden -fvisibility-inlines-hidden \
            -Iinclude
# DF_1_NODELETE (-z nodelete) is required for a VisionAlgo-only plug-in.
# libPlugins.so drops its temporary boost::dll::shared_library after
# loadPluginLibraries(); the Vision loader's stored factory does not keep the
# library mapped, so a Vision-only .so is unloaded after its factory returns.
# Stock SLAM survives because it also exports createActivityPlugin and is
# retained by the Activity path. nodelete keeps this library mapped so
# DetectionProcess vtables remain valid (and makes the load observable in
# /proc/<pid>/maps).
LDFLAGS  := -shared -Wl,-z,nodelete -Wl,-soname,libExampleVisionPlugin.so \
            -pthread -ldl

BUILD := build
TARGET := $(BUILD)/libExampleVisionPlugin.so
SRC := src/example_vision_plugin.cpp src/hearts_controller.cpp
HDRS := include/toadi/unknown.h include/toadi/broker.h \
        include/toadi/vision_algo_plugin.h include/toadi/eyes_ui.h \
        include/toadi/example_hearts.h src/hearts.h \
        src/spidey_frames.h src/spidey2_frames.h src/spider_frames.h \
        src/pumpkin_frames.h src/badge_frames.h src/walle_frames.h

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -pthread $(SRC) $(LDFLAGS) -o $@
	@echo "built $@"
	@file $@

clean:
	rm -rf $(BUILD)

# Copy to a robot or a staging rootfs. Example:
#   make install DESTDIR=/home/toadi/plugins
DESTDIR ?=
install: $(TARGET)
	@test -n "$(DESTDIR)" || (echo "set DESTDIR=/path/to/plugins" >&2; exit 1)
	install -m 0755 $(TARGET) $(DESTDIR)/
	@echo "installed $(DESTDIR)/libExampleVisionPlugin.so"
