# x86Sim: build everything that can be built on Linux. Paths come from cli/lib/config.sh (x86sim.env).
# The simulator-side binaries in host/ are Mach-O: make sdks, then make mac builds them here (docs/DEVELOPMENT.md).

ROOT := $(CURDIR)
cfg = $(shell X86SIM_ROOT='$(ROOT)' sh -c '. cli/lib/config.sh; echo "$$$(1)"')
PREFIX := $(call cfg,X86SIM_PREFIX)
DBUILD := $(call cfg,X86SIM_DARLING_BUILD)

.PHONY: all help submodules mtlvk darling darling-configure darling-install sdks mac mac-diag clean

all: mtlvk darling

help:
	@echo "make submodules         init external/darling (recursive) and external/metal2vulkan"
	@echo "make mtlvk              linux/mtlvk (Rust, Vulkan): Metal stand-in, Linux side"
	@echo "make darling            configure (once) and build Darling in $(DBUILD)"
	@echo "make darling-install    build + install Darling into $(PREFIX) (sudo: bin/darling is setuid root;"
	@echo "                        SUDO_PASS from the environment or a sudo prompt)"
	@echo "make sdks               sdks/MacOSX.sdk from Apple's public CDN + a substitute sdks/iPhoneSimulator.sdk built from"
	@echo "                        the runtime in runtimes/ (no Mac, no Apple ID; build/sdk/, docs/DEVELOPMENT.md)"
	@echo "make mac                build the simulator-side Mach-O binaries (host/*/arm64, simhost): on Linux with sdks/,"
	@echo "                        else on \$$X86SIM_MAC (Xcode, ssh)"
	@echo "make mac-diag           build the optional arm64 diagnostic shims (host/simshim/diag/arm64)"

submodules:
	git submodule sync --recursive
	git submodule update --init external/metal2vulkan
	git submodule update --init --recursive external/dynarmic
	git submodule update --init --recursive external/darling

mtlvk:
	cd linux/mtlvk && cargo build --release

darling-configure:
	@test -f "$(DBUILD)/build.ninja" || cmake -S external/darling -B "$(DBUILD)" -G Ninja \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
		-DCMAKE_INSTALL_PREFIX="$(PREFIX)" -DTARGET_i386=OFF -DCOMPONENTS=stock -DENABLE_METAL=OFF \
		-DCMAKE_C_FLAGS=-fno-objc-constant-literals -DCMAKE_CXX_FLAGS=-fno-objc-constant-literals

darling: darling-configure
	ninja -C "$(DBUILD)"

darling-install: darling-configure
	tools/debug/dinstall.sh

sdks:
	build/sdk/macos-sdk.py
	build/sdk/sim-sdk.py

mac:
	build/mac-build-simhost.sh
	build/mac-build-arm64-shims.sh simshim simaudio simfb simhid metalhostd iosurfaced mtlprobe openurl netconfig fpsim2host

mac-diag:
	build/mac-build-diag.sh

clean:
	cd linux/mtlvk && cargo clean
