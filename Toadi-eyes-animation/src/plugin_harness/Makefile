# Cross-build and run the plug-in ABI harness.
#
#   make abi                 # build build/abi_harness + staged libBroker.so
#   make run                 # build the example plug-in and run the harness
#
# Requires the aarch64 cross sysroot under tools/cross-bullseye/root and the
# extracted robot rootfs under robot_rootfs_full (see spec 100).

CROSS ?= /home/demo/Downloads/toadi_work/tools/cross-bullseye/root
ROBOT ?= /home/demo/Downloads/toadi_work/robot_rootfs_full
ROOTFS ?= $(ROBOT)
LIBS   ?= $(ROBOT)
QEMU  ?= /home/demo/Downloads/toadi_work/tools/qemu-aarch64-static
BUILD ?= build

CXX   := $(CROSS)/usr/bin/aarch64-linux-gnu-g++-10
GCCLIB := $(CROSS)/usr/lib/gcc-cross/aarch64-linux-gnu/10
export LD_LIBRARY_PATH := $(CROSS)/usr/lib/x86_64-linux-gnu:$(LD_LIBRARY_PATH)

CXXFLAGS := --sysroot=$(CROSS) -B$(GCCLIB) -L$(GCCLIB) \
            -std=c++17 -O2 -Wall -Wextra \
            -I../example_plugin/include
LDFLAGS := -ldl -Wl,--allow-shlib-undefined -Wl,-rpath,'$$ORIGIN'

PLUGIN_DIR := ../example_plugin
PLUGIN := $(PLUGIN_DIR)/build/libExampleVisionPlugin.so
ANIM_DIR := ../animation_plugin
ANIM := $(ANIM_DIR)/build/libEyesAnimationPlugin.so
FOLLOW_DIR := ../follow_plugin
FOLLOW := $(FOLLOW_DIR)/build/libFollowPlugin.so

.PHONY: all abi loader hearts follow plugin anim run run-inert loader-run hearts-run follow-run clean

# 'follow' is omitted from 'all': it needs src/follow_plugin from the full
# workspace, which is not part of this publish.
all: abi loader hearts

abi: $(BUILD)/abi_harness $(BUILD)/libBroker.so
loader: $(BUILD)/loader_harness $(BUILD)/libBroker.so
hearts: $(BUILD)/hearts_harness $(BUILD)/libBroker.so
follow: $(BUILD)/follow_harness $(BUILD)/libBroker.so

$(BUILD):
	mkdir -p $@

$(BUILD)/libBroker.so: $(LIBS)/home/toadi/lib/libBroker.so | $(BUILD)
	cp $< $@

$(BUILD)/abi_harness: abi_harness.cpp $(BUILD)/libBroker.so | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -L$(BUILD) -lBroker $(LDFLAGS) -o $@

# Links the real libPlugins.so + libLog.so from the extracted robot rootfs.
# --allow-shlib-undefined lets the older bullseye linker accept their
# GLIBC_2.34/GLIBC_2.35 symbol versions; the binaries run under the robot's
# own glibc via qemu -L.
$(BUILD)/loader_harness: loader_harness.cpp $(BUILD)/libBroker.so | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -L$(BUILD) -lBroker \
	    -L$(LIBS)/home/toadi/lib -lPlugins -lLog \
	    $(LDFLAGS) -o $@

$(BUILD)/hearts_harness: hearts_harness.cpp $(BUILD)/libBroker.so | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(ANIM_DIR)/include -I$(ANIM_DIR)/src $< \
	    -L$(BUILD) -lBroker $(LDFLAGS) -pthread -o $@

$(BUILD)/follow_harness: follow_harness.cpp $(BUILD)/libBroker.so | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(FOLLOW_DIR)/include $< -L$(BUILD) -lBroker \
	    $(LDFLAGS) -pthread -o $@

plugin:
	$(MAKE) -C $(PLUGIN_DIR)

anim:
	$(MAKE) -C $(ANIM_DIR)

# Run under qemu with the robot rootfs as the ELF interpreter/library prefix.
# The plug-in and libBroker.so are copied to the local build dir first: the
# CIFS workspace may be mounted noexec, and dlopen needs PROT_EXEC mappings.
run: abi plugin
	cp $(PLUGIN) $(BUILD)/libExampleVisionPlugin.so
	$(QEMU) -L $(ROOTFS) $(BUILD)/abi_harness $(BUILD)/libExampleVisionPlugin.so

# Same test with the plug-in's broker registration disabled (inert default).
run-inert: abi plugin
	cp $(PLUGIN) $(BUILD)/libExampleVisionPlugin.so
	$(QEMU) -L $(ROOTFS) $(BUILD)/abi_harness $(BUILD)/libExampleVisionPlugin.so --no-service

# Full path: libPlugins.so scans $(BUILD)/plugins, dlopens the example, and
# drives the VisionAlgo plugin interface itself.
loader-run: loader plugin
	rm -rf $(BUILD)/plugins && mkdir -p $(BUILD)/plugins
	cp $(PLUGIN) $(BUILD)/plugins/
	LD_LIBRARY_PATH=$(LIBS)/home/toadi/lib:$(LIBS)/usr/lib \
	    $(QEMU) -L $(ROOTFS) $(BUILD)/loader_harness $(BUILD)/plugins

# Fake eyesui service + REST round-trip; no ToadiApp or SPI involved.
hearts-run: hearts anim
	cp $(ANIM) $(BUILD)/libEyesAnimationPlugin.so
	LD_LIBRARY_PATH=$(LIBS)/home/toadi/lib:$(LIBS)/usr/lib \
	    $(QEMU) -L $(ROOTFS) $(BUILD)/hearts_harness \
	    $(BUILD)/libEyesAnimationPlugin.so

# Follow trigger/supervisor harness; no ToadiApp or motors.
follow-run: follow
	$(MAKE) -C $(FOLLOW_DIR)
	cp $(FOLLOW) $(BUILD)/libFollowPlugin.so
	LD_LIBRARY_PATH=$(LIBS)/home/toadi/lib:$(LIBS)/usr/lib \
	    $(QEMU) -L $(ROOTFS) $(BUILD)/follow_harness \
	    $(BUILD)/libFollowPlugin.so

clean:
	rm -rf $(BUILD)
