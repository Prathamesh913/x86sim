#!/bin/sh
# x86Code release build: everything the AppImage carries, built from source, nothing from Apple.
#   OUT (default build/release/out): all outputs; the source tree is only read (works on a read-only checkout)
#   Darling is configured with the fixed install prefix /opt/x86code/darling (its binaries carry that path: darling
#   execs $PREFIX/bin/darlingserver, darlingserver overlays $PREFIX/libexec/darling) and installed with DESTDIR into
#   $OUT/destdir. The setup wizard's privileged step copies it to /opt/x86code/darling-<version> (root-owned, bin/darling
#   setuid) and points the /opt/x86code/darling symlink at it.
#   JOBS (default nproc), STEPS (default "darling arm64jit arm64host mtlvk launcher")
set -e
X=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(mkdir -p "${OUT:-$X/build/release/out}" && cd "${OUT:-$X/build/release/out}" && pwd)
JOBS=${JOBS:-$(nproc)}
STEPS=${STEPS:-darling arm64jit arm64host mtlvk launcher}
PREFIX=/opt/x86code/darling
DB=$OUT/dbuild
DEST=$OUT/destdir
L=$DEST$PREFIX/libexec/darling/usr/libexec/darling
t0=$(date +%s)
step() { echo "== $1 ($(( $(date +%s) - t0 )) s)"; }
for s in $STEPS; do case $s in
darling)
	step "darling: configure"
	[ -f "$DB/build.ninja" ] || cmake -S "$X/external/darling" -B "$DB" -G Ninja \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" -DTARGET_i386=OFF -DCOMPONENTS=stock -DENABLE_METAL=OFF \
		-DCMAKE_C_FLAGS=-fno-objc-constant-literals -DCMAKE_CXX_FLAGS=-fno-objc-constant-literals \
		${CCACHE:+-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache} > "$OUT/dconfigure.log" 2>&1 \
		|| { tail -40 "$OUT/dconfigure.log"; exit 1; }
	step "darling: build"
	ninja -C "$DB" -j"$JOBS" > "$OUT/dbuild.log" 2>&1 || { grep -E 'FAILED|error:' "$OUT/dbuild.log" | head -30; tail -20 "$OUT/dbuild.log"; exit 1; }
	step "darling: install (DESTDIR)"
	rm -rf "$DEST"
	DESTDIR=$DEST ninja -C "$DB" install > "$OUT/dinstall.log" 2>&1 || { tail -30 "$OUT/dinstall.log"; exit 1; }
	# as tools/debug/dinstall.sh
	mkdir -p "$DEST$PREFIX/libexec/darling/private/var/tmp" && chmod 1777 "$DEST$PREFIX/libexec/darling/private/var/tmp"
	python3 -I "$X/tools/debug/set_macho_platform.py" "$DEST$PREFIX/libexec/darling/usr/lib/darling/libkqueue_sim.dylib" >/dev/null
	;;
arm64jit)
	step "arm64jit"
	B=$OUT/build-arm64jit
	[ -f "$B/build.ninja" ] || cmake -S "$X/linux/arm64jit" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ > "$OUT/arm64jit-cmake.log" 2>&1
	ninja -C "$B" -j"$JOBS" arm64jit a64hugecopy > "$OUT/arm64jit.log" 2>&1 || { grep -E 'FAILED|error:' "$OUT/arm64jit.log" | head; exit 1; }
	mkdir -p "$L" && cp "$B/libarm64jit.so" "$B/a64hugecopy" "$L/"
	;;
arm64host)
	step "arm64host"
	mkdir -p "$L" && ARM64HOST_OUT=$L/arm64host "$X/host/arm64host/build.sh" "$DB"
	;;
mtlvk)
	step "mtlvk"
	(cd "$X/linux/mtlvk" && CARGO_TARGET_DIR=$OUT/cargo-mtlvk cargo build --release --locked -q)
	;;
launcher)
	step "launcher"
	(cd "$X/tools/launcher" && CARGO_TARGET_DIR=$OUT/cargo-launcher cargo build --release --locked -q)
	;;
*) echo "unknown step $s"; exit 1 ;;
esac; done
step done
