# MasterForce: a master / end-of-chain insert effect for MPC OS (Force / MPC standalone).
# Builds on Linux or WSL. Native: g++ (tests, the prototype, x86 bench). Device: arm-linux-gnueabihf-g++ 11
# or newer (libstdc++ is linked dynamically: MPC OS 3.x has GCC 13's, see plugin-package). Releases come from
# CI, built against glibc 2.31, the catalog's limit for MPC OS 2.x and 3.x; a newer distribution's cross toolchain
# needs a newer glibc (3.x only).
# Your own settings (FORCE, SSH_KEY, PY) go in local.mk, which git ignores. (EffectForce's, cut to one chain.)
-include local.mk

CXX      ?= g++
# ARM_PREFIX: the device toolchain's prefix (also for strip, readelf and nm); empty for a native ARM
# build (the release CI builds in arm32v7/gcc:11-bullseye, glibc 2.31, see .github/workflows/build.yml).
# ARM_RUN: how ARM programs run here: qemu-user on x86, nothing on ARM.
ARM_PREFIX ?= arm-linux-gnueabihf-
ARM_CXX  ?= $(ARM_PREFIX)g++
ARM_RUN  ?= qemu-arm -L /usr/arm-linux-gnueabihf
BUILD    := build
# FORCE: the device, root@<ip>, for bench-device and plugin-install. SSH_KEY: the private key for
# it (empty: ssh's own defaults). PY: a Python 3 with Pillow, for skin, preview and plugin-package.
FORCE    ?=
SSH_KEY  ?=
PY       ?= python3

ifneq ($(filter bench-device plugin-install plugin-uninstall,$(MAKECMDGOALS)),)
ifeq ($(strip $(FORCE)),)
$(error set FORCE=root@<device-ip> (on the command line or in local.mk))
endif
endif

# -n: ssh otherwise inherits make's stdin and hangs. No host-key checks: the Force's DHCP
# address changes on every restart, so its key is never in known_hosts.
SSH_OPTS := $(if $(strip $(SSH_KEY)),-i $(SSH_KEY) -o IdentitiesOnly=yes) -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
SSH      := ssh -n $(SSH_OPTS)

MV       := third_party/mpc-vst-plugins
SURF     := surface
SURF_OUT := $(SURF)/build
SKIN_DIR := $(SURF_OUT)/skin/Devko - VST - MasterForce
GEN      := $(SURF_OUT)/param_ids.h $(SURF_OUT)/factory_presets.h
# What the skin is made from (gen_vst.py and skin_polish.py read them; the C++ reads none).
SKIN_IN  := $(SURF)/params.json $(SURF)/layout.conf $(SURF)/vst.json $(SURF_OUT)/skin_style.json
SURF_STAMP := $(SURF_OUT)/surface.stamp
SKIN     := $(SURF_OUT)/skin.stamp

SRC      := $(wildcard dsp/*.cpp) $(wildcard plugin/*.cpp)
HDR      := $(wildcard dsp/*.h plugin/*.h)
INC      := -I$(SURF_OUT) -Iplugin -Itools
TESTS    := $(wildcard test/*.cpp)
THDR     := $(wildcard test/*.h)
DSP_SRC  := $(wildcard dsp/*.cpp)

# -funsafe-math-optimizations: without it GCC won't use NEON for float vectors on ARMv7
# (NEON flushes denormals, so it isn't IEEE-exact). Not -ffast-math: keep isfinite() honest
# (every stage drops NaNs with it).
ARM_OPT  := -O3 -march=armv7-a -mtune=cortex-a17 -mfpu=neon-vfpv4 -mfloat-abi=hard \
            -funsafe-math-optimizations -fno-math-errno -fno-tree-loop-distribute-patterns
ARM_SO   := $(BUILD)/arm/masterforce.so
ARM_BENCH := $(BUILD)/arm/mfbench

.PHONY: all surface skin preview test test-arm test-arm-pgo proto bench arm-plugin arm-bench bench-device rebuild-check \
        plugin-package plugin-install plugin-uninstall clean
# A recipe that fails leaves no half-written target behind for the next make to trust.
.DELETE_ON_ERROR:
all: test arm-plugin

$(BUILD):
	mkdir -p $@

# --- generated --------------------------------------------------------------------------------
# surface.py: params.json, layout.conf, vst.json and build/param_ids.h (the C++ side). Needs only
# python3, so tests and the .so build anywhere. It also checks the layout and every factory preset
# before writing anything, and writes a file only when its text changes: what is built from one is rebuilt only
# then (a preset edit rebuilds what includes the presets header, not the skin). A stamp records the run.
surface: $(GEN) $(SKIN_IN)
# The preset folders themselves too: their times change when a preset is deleted.
# The fonts too: the layout check measures labels with them.
$(SURF_STAMP): $(SURF)/surface.py presets/Factory $(wildcard presets/Factory/*) $(wildcard presets/Factory/*/*.mfp) \
               $(wildcard $(SURF)/fonts/*.ttf)
	python3 $(SURF)/surface.py
	touch $@
# Up to date once surface.py has run, unless deleted by hand since.
$(GEN) $(SKIN_IN): $(SURF_STAMP)
	@[ -f $@ ] || python3 $(SURF)/surface.py

# The skin (TUI.json + PNGs) and the plugin-list entry: sd88me's generator, Pillow and a host gcc.
# Then surface/skin_polish.py redraws the knob strips, trigger buttons and stepper arrows (same names
# and sizes) and makes each group's pages sub-pages of one tab; it checks the skin first and fails the
# build on a mismatch. Before that its selftest polishes a fabricated skin for this layout and checks the
# images and the refusals. SHADOW_TITLE_FONT = TITLE_FONT in surface.py.
skin: $(SKIN)
$(SKIN): $(SKIN_IN) $(MV)/tools/gen_vst.py $(MV)/tools/shadow_skin.py $(MV)/tools/skin_assets.py $(MV)/tools/shadow_art.c \
         $(MV)/tools/params.py $(wildcard $(MV)/tools/vendor/force-shadow/*/*) $(SURF)/skin_polish.py $(wildcard $(SURF)/fonts/*.ttf)
	mkdir -p $(SURF_OUT)
	$(PY) $(SURF)/skin_polish.py --selftest
	gcc -O2 -I$(MV)/tools/vendor/force-shadow/tools -o $(SURF_OUT)/shadow_art $(MV)/tools/shadow_art.c -lm
	cd $(SURF) && SHADOW_TITLE_FONT=fonts/TitilliumWeb-Bold.ttf $(PY) ../$(MV)/tools/gen_vst.py vst.json
	$(PY) $(SURF)/skin_polish.py "$(SKIN_DIR)/Plugin Skins" --layout $(SURF)/layout.conf --style $(SURF_OUT)/skin_style.json
	touch $@

# Skin previews (PNG per page) for checking the layout without a device.
preview: $(SKIN)
	$(PY) $(MV)/tools/studio.py preview "$(SKIN_DIR)/Plugin Skins" -o $(SURF_OUT)/page_%d.png

# --- native -----------------------------------------------------------------------------------
# Every stage of the chain and the whole plugin through its VST2 entry points, under ASan/UBSan;
# any undefined behaviour fails the run (UBSan otherwise reports and carries on).
test: $(BUILD)/mf_test
	$(BUILD)/mf_test

$(BUILD)/mf_test: $(TESTS) $(THDR) $(SRC) $(HDR) $(GEN) | $(BUILD)
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Wall -Wextra -pthread \
		$(INC) $(SRC) $(TESTS) -o $@

# The same suite cross-compiled for the Force's CPU and run under qemu-user (no sanitizers):
# catches 32-bit and ARM-only code paths (the FPSCR flush, NEON float code).
test-arm: $(BUILD)/arm/mf_test
	$(ARM_RUN) $<

$(BUILD)/arm/mf_test: $(TESTS) $(THDR) $(SRC) $(HDR) $(GEN)
	mkdir -p $(BUILD)/arm
	$(ARM_CXX) -std=c++17 $(ARM_OPT) -Wall -Wextra -Wno-psabi -pthread $(INC) $(SRC) $(TESTS) -o $@

# The offline prototype (docs/DESIGN.md "Plan"): build/mfproto analyze | master (tools/mfproto.cpp says how).
proto: $(BUILD)/mfproto
$(BUILD)/mfproto: tools/mfproto.cpp tools/wav.h $(DSP_SRC) $(wildcard dsp/*.h) | $(BUILD)
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(DSP_SRC) tools/mfproto.cpp -o $@

# x86 numbers say nothing about the Force; this only checks the bench and the .so path work.
bench: $(BUILD)/masterforce.so $(BUILD)/mfbench
	$(BUILD)/mfbench $(BUILD)/masterforce.so -s 1 -c -1

$(BUILD)/masterforce.so: $(SRC) $(HDR) $(GEN) plugin/exports.map | $(BUILD)
	$(CXX) -std=c++17 -O3 -fno-tree-loop-distribute-patterns -fPIC -fvisibility=hidden -fno-gnu-unique -Wall -Wextra -pthread \
		$(INC) -shared -Wl,--no-undefined -Wl,--version-script=plugin/exports.map $(SRC) -o $@

$(BUILD)/mfbench: tools/bench.cpp $(HDR) $(GEN) | $(BUILD)
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(INC) $< -ldl -o $@

# --- device -----------------------------------------------------------------------------------
# The .so MPC loads: only VSTPluginMain exported (a version script hides the C++ template
# instantiations and typeinfo -fvisibility leaves; -fno-gnu-unique keeps it unloadable);
# --no-undefined because an unresolved symbol otherwise only shows up as MPC crashing on load.
ARM_SO_FLAGS = -std=c++17 $(ARM_OPT) -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -fno-gnu-unique -Wall -Wextra -Wno-psabi -pthread $(INC)
ARM_SO_LINK  = -shared -Wl,--no-undefined -Wl,-soname,masterforce.so -Wl,--version-script=plugin/exports.map

ARM_SO_CMD   = $(ARM_CXX) $(ARM_SO_FLAGS) $(ARM_SO_LINK)

# Profile-guided: on by default when ARM programs can run here (qemu-arm installed, as test-arm
# needs, or a native ARM build); PGO=0 builds without. A copy of the plugin compiled with counters
# is linked into tools/pgo_train.cpp, which plays the reference mix through it (a LEARN, every choice,
# every preset); then the .so is compiled from the same sources with the same flags plus that profile.
# -fprofile-partial-training keeps functions the trainer never ran optimised as usual. Objects keep one
# path (dir_name.o) in both rounds: GCC names the profile files after it. A missing profile fails the
# build instead of quietly building without. (SubForce's.)
PGO      ?= auto
ARM_RUNS := $(if $(strip $(ARM_RUN)),$(shell command -v $(firstword $(ARM_RUN)) 2>/dev/null),native)
PGO_ON   := $(if $(filter auto,$(PGO)),$(if $(ARM_RUNS),1,0),$(PGO))
PGO_DIR  := $(BUILD)/arm/pgo
PGO_PROF := $(abspath $(PGO_DIR)/profile)
PGO_OBJ  := $(PGO_DIR)/obj
PGO_O    = $(PGO_OBJ)/$$(echo $$f | tr / _ | sed 's/\.cpp$$/.o/')

# The .so is rebuilt when the way it is built changes (PGO on/off, the compiler and its version, flags, link
# options), not only its sources.
ARM_SO_STAMP := $(BUILD)/arm/so_flags
$(ARM_SO_STAMP): rebuild-check
	@mkdir -p $(dir $@)
	@{ echo '$(PGO_ON) $(ARM_SO_CMD)'; $(ARM_CXX) --version 2>/dev/null | head -1; } > $@.new; \
		if cmp -s $@.new $@; then rm $@.new; else mv $@.new $@; fi
rebuild-check:

arm-plugin: $(ARM_SO)
$(ARM_SO): $(SRC) $(HDR) $(GEN) tools/pgo_train.cpp tools/reference_mix.h plugin/exports.map $(ARM_SO_STAMP)
	mkdir -p $(BUILD)/arm
ifeq ($(PGO_ON),1)
	rm -rf $(PGO_DIR) && mkdir -p $(PGO_OBJ) $(PGO_PROF)
	for f in $(SRC); do $(ARM_CXX) $(ARM_SO_FLAGS) -fprofile-generate=$(PGO_PROF) -fprofile-update=prefer-atomic \
		-c $$f -o $(PGO_O) || exit 1; done
	$(ARM_CXX) $(ARM_SO_FLAGS) -fprofile-generate tools/pgo_train.cpp $(PGO_OBJ)/*.o -o $(PGO_DIR)/train
	MF_DATA_DIR=$(PGO_DIR) MF_PRESET_ROOTS=$(PGO_DIR) MF_FIXED_SEED=1 $(ARM_RUN) $(PGO_DIR)/train
	@n=$$(ls $(PGO_PROF)/*.gcda 2>/dev/null | wc -l); [ $$n -eq $(words $(SRC)) ] || \
		{ echo "PGO: $$n of $(words $(SRC)) profiles written (PGO=0 builds without)"; exit 1; }
	for f in $(SRC); do $(ARM_CXX) $(ARM_SO_FLAGS) -fprofile-use=$(PGO_PROF) -fprofile-partial-training -Werror=missing-profile \
		-c $$f -o $(PGO_O) || exit 1; done
	$(ARM_CXX) $(ARM_SO_FLAGS) $(ARM_SO_LINK) $(PGO_OBJ)/*.o -o $@
	@echo "profile-guided build"
else
	$(ARM_SO_CMD) $(SRC) -o $@
	@echo "plain build (PGO=$(PGO): $(firstword $(ARM_RUN)) $(if $(ARM_RUNS),found,not found))"
endif
	$(ARM_PREFIX)strip --strip-unneeded $@
	@v=$$($(ARM_PREFIX)readelf -V $@); echo needs $$(for p in GLIBC GLIBCXX CXXABI; do \
		echo "$$v" | grep -o "$${p}_[0-9][0-9.]*" | sort -uV | tail -1; done)
	@n=$$($(ARM_PREFIX)nm -D --defined-only $@ | wc -l); echo "exported symbols: $$n"; \
		[ $$n -eq 1 ] || { $(ARM_PREFIX)nm -D --defined-only $@; echo "only VSTPluginMain may be exported"; exit 1; }

# The suite against the objects the shipped .so is linked from (profile-guided), under qemu. Here, below
# PGO_ON: make reads ifeq when it reads the line, so above its definition it was always the plain branch.
test-arm-pgo: $(ARM_SO)
ifeq ($(PGO_ON),1)
	$(ARM_CXX) -std=c++17 $(ARM_OPT) -Wno-psabi -pthread $(INC) $(TESTS) $(PGO_OBJ)/*.o -o $(BUILD)/arm/mf_test_pgo
	$(ARM_RUN) $(BUILD)/arm/mf_test_pgo
else
	@echo "test-arm-pgo: the .so is a plain build here (PGO=$(PGO)); test-arm covers it"
endif

arm-bench: $(ARM_BENCH)
$(ARM_BENCH): tools/bench.cpp $(HDR) $(GEN)
	mkdir -p $(BUILD)/arm
	$(ARM_CXX) -std=c++17 $(ARM_OPT) -Wall -Wextra -Wno-psabi $(INC) $< -ldl -o $@

# Bench on the Force: copies the .so and the bench to /tmp, runs pinned to core 1 (MPC's audio
# workers own cores 2-3) while MPC keeps running, then deletes them. Touches nothing else.
#   make bench-device FORCE=root@<ip>
BENCH_ARGS ?= -s 3
bench-device: $(ARM_SO) $(ARM_BENCH)
	scp -q $(SSH_OPTS) $(ARM_SO) $(ARM_BENCH) $(FORCE):/tmp/
	$(SSH) $(FORCE) 'r=0; /tmp/mfbench /tmp/masterforce.so $(BENCH_ARGS) -c 1 || r=1; \
		rm -f /tmp/mfbench /tmp/masterforce.so; exit $$r'

# Release zip: plugin + skin + sd88me's installer (stops MPC, backs up and edits MPC.settings,
# restarts MPC). The plugin-list entry says Effect with 2 inputs: MPC lists it under insert effects.
PLUGIN_VERSION ?= 0.0.1
PKG      := MasterForce-$(PLUGIN_VERSION)
plugin-package: $(ARM_SO) $(SKIN)
	@# Everything shipped runs under BusyBox on the device: a CR in a script breaks it there.
	@# grep: 1 = no CR found (good); 0 = found one; 2 = it couldn't read the scripts.
	@grep -l "$$(printf '\r')" $(MV)/tools/release/*; r=$$?; [ $$r -eq 1 ] || { echo "error: CRLF in a shipped script, or no scripts"; exit 1; }
	@# A newer toolchain's glibc: fine on the Force (MPC OS 3.x), not for a release (the catalog's limit is 2.32).
	@g=$$(readelf -V $(ARM_SO) 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1); \
		if [ -n "$$g" ] && [ "$$(printf '%s\n2.32\n' "$$g" | sort -V | tail -1)" != 2.32 ]; then \
		echo "warning: this .so needs glibc $$g: it loads on MPC OS 3.x only, and the plugin catalog refuses it."; \
		echo "         Release packages come from CI (glibc 2.31)."; fi
	@# libstdc++, which the catalog's checker doesn't read: the Force's MPC OS 3.x has GCC 13's (libstdc++.so.6.0.32,
	@# GLIBCXX_3.4.32, CXXABI_1.3.14), and a .so that needs more loads on none. Every GCC 11 build needs GLIBCXX_3.4.29:
	@# float to_chars/from_chars (plugin/state.cpp), and GCC 11's std::allocator itself (__throw_bad_array_new_length).
	@# Whether MPC OS 2.x has it is unknown (its glibc 2.32 is from GCC 10's time, 3.4.28): the README asks for 3.x.
	@v=$$(readelf -V $(ARM_SO)); for max in GLIBCXX_3.4.32 CXXABI_1.3.14; do p=$${max%_*}; \
		n=$$(echo "$$v" | grep -o "$${p}_[0-9][0-9.]*" | sed "s/^$${p}_//" | sort -uV | tail -1); \
		if [ -n "$$n" ] && [ "$$(printf '%s\n%s\n' "$$n" "$${max#*_}" | sort -V | tail -1)" != "$${max#*_}" ]; then \
		echo "error: this .so needs $${p}_$$n, newer than the Force's libstdc++ (up to $$max): it won't load"; exit 1; fi; done
	$(PY) $(MV)/tools/release.py --so $(ARM_SO) --skin "$(SKIN_DIR)" --entry $(SURF_OUT)/pluginlist-entry.xml \
		--version $(PLUGIN_VERSION) --repo Devko/MasterForce --license MIT \
		--about "MasterForce master / end-of-chain (preview): a true-peak limiter at -1 dBTP that never lets the master clip, and a one-press LEARN: play the loudest 8 bars and it sets a broad tone, gentle glue and the gain for YouTube (-14 LUFS) or a louder target, with warmth, width and mono bass." \
		--requires "root SSH (MockbaMod)" \
		--user-data Presets --user-data preset_favorites.txt --user-data preset_recent.txt \
		-o dist

# Install on a device: stops MPC, backs up + edits MPC.settings, restarts MPC. Save the MPC
# project first. Usage: make plugin-install FORCE=root@<ip>
PKG_TMP := /tmp/mfpkg
plugin-install: plugin-package
	rm -rf $(PKG_TMP) && mkdir -p $(PKG_TMP)
	python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' dist/$(PKG)-mpc-armv7.zip $(PKG_TMP)
	tar -C $(PKG_TMP) -cf - $(PKG) | ssh $(SSH_OPTS) $(FORCE) 'rm -rf /tmp/$(PKG) && tar -C /tmp -xf -'
	$(SSH) $(FORCE) 'sh /tmp/$(PKG)/install.sh -y'

# Remove it again (also stops and restarts MPC); keeps the user's presets.
plugin-uninstall: plugin-package
	rm -rf $(PKG_TMP) && mkdir -p $(PKG_TMP)
	python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' dist/$(PKG)-mpc-armv7.zip $(PKG_TMP)
	tar -C $(PKG_TMP) -cf - $(PKG) | ssh $(SSH_OPTS) $(FORCE) 'rm -rf /tmp/$(PKG) && tar -C /tmp -xf -'
	$(SSH) $(FORCE) 'sh /tmp/$(PKG)/uninstall.sh -y'

clean:
	rm -rf $(BUILD) $(SURF_OUT) dist
