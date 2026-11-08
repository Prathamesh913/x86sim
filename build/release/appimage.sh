#!/bin/sh
# x86Code release: assemble the AppDir from build/release/build.sh's outputs and the checkout, then pack the AppImage.
#   OUT        build.sh's output dir (default build/release/out); the AppDir and the AppImage land there
#   VERSION    release version (default: git describe of the checkout, e.g. v0.1.0-pre1-3-gabc1234)
#   UPDATE_INFO  AppImage update information (default: GitHub releases of x86code/x86code, pre-releases included,
#              zsync); "none" for no update info
# Nothing from Apple goes in: the payload is `git ls-files` of our own dirs (the committed simulator-side binaries are
# ours, built from host/ sources) plus build outputs; check_no_apple below refuses a tree with Apple runtime/SDK files.
# Third-party tools are downloaded pinned (sha256 checked): ipsw (MIT), 7-Zip's 7zz (LGPL-2.1 + unRAR), dnspython (ISC).
set -e
X=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(cd "${OUT:-$X/build/release/out}" && pwd)
VERSION=${VERSION:-$(git -C "$X" describe --tags --always --dirty 2>/dev/null || echo 0.0.0-dev)}
VERSION=${VERSION#v}
UPDATE_INFO=${UPDATE_INFO:-gh-releases-zsync|x86code|x86code|latest-pre|x86code-*-x86_64.AppImage.zsync}
DL=$OUT/downloads
A=$OUT/AppDir
S=$A/usr/share/x86code
P=$S/payload
DEST=$OUT/destdir/opt/x86code/darling
mkdir -p "$DL"

IPSW_VER=3.1.741
IPSW_SHA=${IPSW_SHA:-992a93c708861c14118ebd69c3493d2f5d0af08f57fb345839630ece3e227b20}
SZ_VER=2604
SZ_SHA=${SZ_SHA:-fc0327ba27e89bd086cf426dff17d77de582953cdbbc10a6576540a06853ffcd}
DNS_VER=2.8.0
DNS_SHA=${DNS_SHA:-01d9bbc4a2d76bf0db7c1f729812ded6d912bd318d3b1cf81d30c0f845dbf3af}

fetch() {  # <url> <file> [sha256]: cached in downloads/, checked when a hash is given, the hash printed otherwise
	if [ ! -s "$DL/$2" ]; then curl -fsSL --retry 3 -o "$DL/$2.part" "$1" && mv "$DL/$2.part" "$DL/$2"; fi
	h=$(sha256sum "$DL/$2" | cut -d' ' -f1)
	if [ -n "$3" ] && [ "$3" != "$h" ]; then echo "appimage: $2: sha256 $h, expected $3" >&2; exit 1; fi
	echo "  $2 sha256 $h"
}

[ -x "$DEST/bin/darling" ] || { echo "appimage: no Darling in $OUT/destdir (run build/release/build.sh)" >&2; exit 1; }
rm -rf "$A"
mkdir -p "$P" "$S/bin" "$S/pylib"

echo "== payload (version $VERSION)"
# our files, as committed (no gitignored Apple data can come along)
git -C "$X" ls-files -z -- cli host linux tools/debug tools/runtime tests docs/RELEASE.md README.md LICENSE* \
	| (cd "$X" && xargs -0 cp --parents -t "$P" 2>/dev/null) || true
rm -rf "$P/linux/mtlvk/src" "$P/linux/arm64run" "$P/tests/metal/shaders"
# build outputs at the paths the scripts and the launcher expect (repo-relative)
mkdir -p "$P/linux/mtlvk/target/release" "$P/tools/launcher/target/release"
cp "$OUT/cargo-mtlvk/release/mtlvk" "$P/linux/mtlvk/target/release/"
for b in x86code-launcher x86code-aot x86code-mcp; do cp "$OUT/cargo-launcher/release/$b" "$P/tools/launcher/target/release/"; done
strip --strip-debug "$P/linux/mtlvk/target/release/mtlvk" "$P/tools/launcher/target/release/"*
echo "$VERSION" > "$S/VERSION"

echo "== Darling (installed by the setup wizard into /opt/x86code)"
cp -a "$DEST" "$S/darling"
# libarm64jit.so keeps .symtab: the launcher reads kCodegenVersion from its g_build symbol (core/src/key.rs)
strip --strip-debug "$S/darling/libexec/darling/usr/libexec/darling/libarm64jit.so" "$S/darling/bin/darlingserver" 2>/dev/null || true
# content id: a new AppImage reinstalls Darling (one password prompt) only when Darling itself changed
DID=$(cd "$S/darling" && find . -type f -o -type l | LC_ALL=C sort | while read -r f; do
	if [ -L "$f" ]; then echo "$f -> $(readlink "$f")"; else echo "$f $(stat -c %a "$f") $(sha256sum < "$f" | cut -c1-16)"; fi
done | sha256sum | cut -c1-16)
echo "$DID" > "$S/DARLING_ID"
cp "$X/build/release/install-helper.sh" "$X/build/release/darling-userns.sh" "$S/"
echo "  darling id $DID, $(du -sh "$S/darling" | cut -f1)"

echo "== tools"
fetch "https://github.com/blacktop/ipsw/releases/download/v$IPSW_VER/ipsw_${IPSW_VER}_linux_x86_64.tar.gz" ipsw.tar.gz "$IPSW_SHA"
mkdir -p "$OUT/tmp-ipsw" && tar xzf "$DL/ipsw.tar.gz" -C "$OUT/tmp-ipsw" && cp "$(find "$OUT/tmp-ipsw" -name ipsw -type f | head -1)" "$S/bin/ipsw"
cp "$(find "$OUT/tmp-ipsw" -iname 'LICENSE*' | head -1)" "$S/bin/LICENSE.ipsw" 2>/dev/null || true
rm -rf "$OUT/tmp-ipsw"
fetch "https://www.7-zip.org/a/7z$SZ_VER-linux-x64.tar.xz" 7z.tar.xz "$SZ_SHA"
mkdir -p "$OUT/tmp-7z" && tar xJf "$DL/7z.tar.xz" -C "$OUT/tmp-7z" && cp "$OUT/tmp-7z/7zz" "$S/bin/7zz" && cp "$OUT/tmp-7z/License.txt" "$S/bin/LICENSE.7zip"
rm -rf "$OUT/tmp-7z"
# runtime-fetch.py runs `7z`
ln -s 7zz "$S/bin/7z"
DNS_URL=$(curl -fsSL "https://pypi.org/pypi/dnspython/$DNS_VER/json" | python3 -c 'import json,sys; print(next(u["url"] for u in json.load(sys.stdin)["urls"] if u["filename"].endswith(".whl")))')
fetch "$DNS_URL" dnspython.whl "$DNS_SHA"
python3 -c 'import sys,zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$DL/dnspython.whl" "$S/pylib"

echo "== licenses"
mkdir -p "$S/licenses"
cp "$X/external/darling/LICENSE" "$S/licenses/darling-LICENSE" 2>/dev/null || true
cp "$X/external/dynarmic/LICENSE.txt" "$S/licenses/dynarmic-LICENSE" 2>/dev/null || true
for f in "$X"/external/metal2vulkan/LICENSE*; do [ -f "$f" ] && cp "$f" "$S/licenses/metal2vulkan-$(basename "$f")"; done
cp "$X/linux/arm64jit/third_party/astcenc/LICENSE.txt" "$S/licenses/astcenc-LICENSE" 2>/dev/null || true

echo "== Apple content check"
# no Apple runtime / SDK / host data: none of their marker paths, no dyld shared cache
bad=$(find "$A" \( -name 'dyld_sim_shared_cache_*' -o -name '*.simruntime' -o -name 'iPhoneSimulator.sdk' -o -name 'MacOSX.sdk' \
	-o -name 'GeoServicesData' -o -name 'RuntimeRoot' -o -path '*/CoreSimulator/Profiles/*' \) -print | head -5)
[ -z "$bad" ] || { echo "appimage: refusing, Apple content found:"; echo "$bad"; exit 1; } >&2

echo "== AppDir"
cp "$X/build/release/AppRun" "$A/AppRun"
chmod +x "$A/AppRun"
cp "$X/build/release/x86code.desktop" "$A/x86code.desktop"
cp "$X/build/release/x86code.png" "$A/x86code.png"
mkdir -p "$A/usr/share/icons/hicolor/256x256/apps" "$A/usr/share/applications"
cp "$X/build/release/x86code.png" "$A/usr/share/icons/hicolor/256x256/apps/"
cp "$X/build/release/x86code.desktop" "$A/usr/share/applications/"
ln -s x86code.png "$A/.DirIcon"

echo "== pack"
fetch https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage appimagetool.AppImage "${APPIMAGETOOL_SHA:-}"
chmod +x "$DL/appimagetool.AppImage"
NAME=x86code-$VERSION-x86_64.AppImage
rm -f "$OUT/$NAME" "$OUT/$NAME.zsync"
set -- --comp zstd --mksquashfs-opt -Xcompression-level --mksquashfs-opt 19 --no-appstream
[ "$UPDATE_INFO" = none ] || set -- "$@" -u "$UPDATE_INFO"
(cd "$OUT" && ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$DL/appimagetool.AppImage" "$@" "$A" "$OUT/$NAME") > "$OUT/appimagetool.log" 2>&1 \
	|| { tail -30 "$OUT/appimagetool.log"; exit 1; }
(cd "$OUT" && sha256sum "$NAME" $( [ -f "$NAME.zsync" ] && echo "$NAME.zsync") > SHA256SUMS)
echo "$OUT/$NAME ($(du -h "$OUT/$NAME" | cut -f1))"
