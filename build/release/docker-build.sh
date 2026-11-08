#!/bin/sh
# Runs build/release/build.sh (then appimage.sh) in the Ubuntu 24.04 image, the checkout mounted read-only, as the
# CI does on its runner. Outputs in build/release/out (gitignored). JOBS limits the parallelism.
set -e
X=$(cd "$(dirname "$0")/../.." && pwd)
O=$X/build/release/out; mkdir -p "$O" "$O/cargo-home"
docker build -q -t x86code-release "$X/build/release"
docker run --rm -u "$(id -u):$(id -g)" -v "$X:/src:ro" -v "$O:/src/build/release/out" \
	-e JOBS="${JOBS:-8}" -e STEPS -e HOME=/src/build/release/out -e CARGO_HOME=/src/build/release/out/cargo-home \
	-e RUSTUP_HOME=/opt/rustup -e OUT=/src/build/release/out x86code-release \
	sh -c 'export PATH=/opt/cargo/bin:$PATH; /src/build/release/build.sh'"${APPIMAGE:+ && /src/build/release/appimage.sh}"
