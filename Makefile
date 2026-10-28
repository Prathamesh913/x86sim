# x86Code: build everything that can be built on Linux. Paths come from cli/lib/config.sh (x86sim.env).
# The simulator-side binaries in host/ and tests/ are Mach-O and are built on a Mac (make mac, see README.md).

ROOT := $(CURDIR)
cfg = $(shell X86SIM_ROOT='$(ROOT)' sh -c '. cli/lib/config.sh; echo "$$$(1)"')
PREFIX := $(call cfg,X86SIM_PREFIX)
DBUILD := $(call cfg,X86SIM_DARLING_BUILD)

.PHONY: all help submodules mtlvk darling darling-configure darling-install mac mac-diag test probes smoke clean

all: mtlvk darling

help:
	@echo "make submodules         init external/darling (recursive) and external/metal2vulkan"
	@echo "make mtlvk              linux/mtlvk (Rust, Vulkan): Metal stand-in, Linux side"
	@echo "make darling            configure (once) and build Darling in $(DBUILD)"
	@echo "make darling-install    build + install Darling into $(PREFIX) (sudo: bin/darling is setuid root;"
	@echo "                        SUDO_PASS from the environment or a sudo prompt)"
	@echo "make mac                build host/ and tests/ Mach-O binaries on \$$X86SIM_MAC (Xcode, ssh)"
	@echo "make mac-diag           build the optional diagnostic shims (host/simshim/diag)"
	@echo "make probes             Metal and IOSurface probes (boot their own minimal device)"
	@echo "make smoke              smoke test on the running boot (or boots one)"

submodules:
	git submodule sync --recursive
	git submodule update --init external/metal2vulkan
	git submodule update --init --recursive external/darling

mtlvk:
	cd linux/mtlvk && cargo build --release

darling-configure:
	@test -f "$(DBUILD)/build.ninja" || cmake -S external/darling -B "$(DBUILD)" -G Ninja \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
		-DCMAKE_INSTALL_PREFIX="$(PREFIX)" -DTARGET_i386=OFF -DCOMPONENTS=stock

darling: darling-configure
	ninja -C "$(DBUILD)"

darling-install: darling-configure
	tools/debug/dinstall.sh

mac:
	build/mac-build-simhost.sh
	build/mac-build-simfb.sh
	build/mac-build-simhid.sh
	build/mac-build-simshim.sh
	build/mac-build-iosurface.sh
	build/mac-build-metal.sh
	build/mac-build-fpsim2host.sh
	build/mac-build-fseventsd.sh
	build/mac-build-jobs.sh

mac-diag:
	build/mac-build-diag.sh

probes:
	tests/metal/run-probe.sh
	tests/iosurface/run-probe.sh

test: probes smoke

smoke:
	tests/smoke.sh

clean:
	cd linux/mtlvk && cargo clean
