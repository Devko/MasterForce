# MasterForce: a master / end-of-chain insert for MPC OS (Force), its DSP and the offline prototype
# for now (docs/DESIGN.md, "Plan"). Builds on Linux or WSL: g++ for the tests and the prototype,
# arm-linux-gnueabihf-g++ and qemu-arm for the suite on the Force's instruction set.
# Your own settings go in local.mk, which git ignores.
-include local.mk

CXX        ?= g++
ARM_PREFIX ?= arm-linux-gnueabihf-
ARM_CXX    ?= $(ARM_PREFIX)g++
ARM_RUN    ?= qemu-arm -L /usr/arm-linux-gnueabihf
BUILD      := build

DSP   := $(wildcard dsp/*.cpp)
HDR   := $(wildcard dsp/*.h)
TESTS := $(wildcard test/*.cpp)
THDR  := $(wildcard test/*.h)

# As EffectForce's: -funsafe-math-optimizations lets GCC use NEON for float vectors on ARMv7; not
# -ffast-math, which would make isfinite() lie (every stage drops NaNs with it).
ARM_OPT := -O3 -march=armv7-a -mtune=cortex-a17 -mfpu=neon-vfpv4 -mfloat-abi=hard \
           -funsafe-math-optimizations -fno-math-errno -fno-tree-loop-distribute-patterns

.PHONY: all test test-arm proto clean
.DELETE_ON_ERROR:
all: test proto

$(BUILD) $(BUILD)/arm:
	mkdir -p $@

# The suite under ASan and UBSan.
test: $(BUILD)/mf_test
	$<
$(BUILD)/mf_test: $(DSP) $(HDR) $(TESTS) $(THDR) | $(BUILD)
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Wall -Wextra \
	    $(DSP) $(TESTS) -o $@

# The suite built for the Force, under qemu-arm.
test-arm: $(BUILD)/arm/mf_test
	$(ARM_RUN) $<
$(BUILD)/arm/mf_test: $(DSP) $(HDR) $(TESTS) $(THDR) | $(BUILD)/arm
	$(ARM_CXX) -std=c++17 $(ARM_OPT) -Wall -Wextra -Wno-psabi $(DSP) $(TESTS) -o $@

# The offline prototype: build/mfproto analyze | master (tools/mfproto.cpp says how).
proto: $(BUILD)/mfproto
$(BUILD)/mfproto: tools/mfproto.cpp tools/wav.h $(DSP) $(HDR) | $(BUILD)
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(DSP) tools/mfproto.cpp -o $@

clean:
	rm -rf $(BUILD)
