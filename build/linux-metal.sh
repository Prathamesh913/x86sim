#!/bin/bash
# x86Code: Linux drop-in for `xcrun -sdk iphonesimulator metal` / `metallib`, served from Mac-built artifacts.
#
# There is no faithful way to produce Apple AIR on Linux: the Metal Shading Language front end exists only in Apple's
# toolchain, and AIR bitcode re-encoded by upstream LLVM (23) is rejected by Apple's MTLCompilerService (it loads as a
# library, every pipeline build crashes the compiler service), while the metallib container itself is easy to write.
# So this tool is a content-addressed cache: the key is the tool, its flags and the bytes of every input; a hit
# copies the byte-identical Mac output, a miss fails (or, with X86CODE_METAL_RECORD=1, runs the real tool on
# $X86SIM_MAC once and stores the result under build/metal-cache/, which is committed).
#
# Usage (same arguments the Mac invocations use; a leading `-sdk <sdk>` is accepted and ignored):
#   build/linux-metal.sh metal    -target air64-apple-ios26.5-simulator -c probe.metal -o probe.air
#   build/linux-metal.sh metal    -target air64-apple-ios26.5-simulator probe.metal -o probe.metallib
#   build/linux-metal.sh metallib probe.air -o probe.metallib
#   build/linux-metal.sh key      metal|metallib <args...>     # print the cache key (to seed the cache by hand)
#   build/linux-metal.sh inspect  <file.metallib>...           # header, sections, functions (type, hash check)
# Also works when symlinked as `metal` / `metallib`.
set -euo pipefail
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
CACHE=${X86CODE_METAL_CACHE:-$ROOT/build/metal-cache}

inspect() {
	python3 -I - "$@" <<'PY'
import struct, sys, hashlib
TYPES = {0: "vertex", 1: "fragment", 2: "kernel", 3: "unqualified", 4: "visible", 5: "extern", 6: "intersection"}
for path in sys.argv[1:]:
    b = open(path, "rb").read()
    if b[:4] != b"MTLB":
        print(f"{path}: not a metallib (magic {b[:4]!r})"); continue
    plat, vmaj, vmin, ftype, osv, osmaj, osmin = struct.unpack_from("<HHHBBHH", b, 4)
    size, = struct.unpack_from("<Q", b, 0x10)
    secs = [struct.unpack_from("<QQ", b, 0x18 + 16 * i) for i in range(4)]
    print(f"{path}: {len(b)} bytes (header says {size}), platform {plat}, format {vmaj}.{vmin}, file type {ftype}, "
          f"os {osv & 0x7f} {osmaj}.{osmin}")
    print("  sections funclist/public/private/bitcode:", ", ".join(f"{o}+{s}" for o, s in secs))
    fo, _ = secs[0]; bo = secs[3][0]
    n, = struct.unpack_from("<I", b, fo); p = fo + 4
    for _ in range(n):
        ln, = struct.unpack_from("<I", b, p); q, end, tags = p + 4, p + ln, {}
        while q < end:
            t = b[q:q + 4]; q += 4
            if t == b"ENDT": break
            sz, = struct.unpack_from("<H", b, q); q += 2; tags[t.decode("latin1")] = b[q:q + sz]; q += sz
        p = end
        name = tags.get("NAME", b"?").rstrip(b"\0").decode()
        ty = TYPES.get(tags["TYPE"][0], tags["TYPE"][0]) if "TYPE" in tags else "?"
        vers = struct.unpack("<4H", tags["VERS"]) if len(tags.get("VERS", b"")) == 8 else None
        ok, sz = "?", 0
        if "OFFT" in tags and "MDSZ" in tags and "HASH" in tags:
            bc = bo + struct.unpack("<QQQ", tags["OFFT"])[2]; sz, = struct.unpack("<Q", tags["MDSZ"])
            body = b[bc:bc + sz]
            ok = "ok" if hashlib.sha256(body).digest() == tags["HASH"] else "MISMATCH"
        print(f"  {name:16} {ty:9} air {vers[0]}.{vers[1]} lang {vers[2]}.{vers[3]}" if vers else f"  {name:16} {ty:9}",
              f"bitcode {sz} B sha256 {ok}", sep="  ")
PY
}

# Parse the tool's arguments: inputs, output (-o), and the flags that define the output.
parse() {
	tool=$1; shift; inputs=(); flags=(); out=""
	while (($#)); do case $1 in
		-sdk) shift 2 ;;
		-o) out=$2; shift 2 ;;
		-o*) out=${1#-o}; shift ;;
		-target|-std|-I|-D|-include|-frameworks) flags+=("$1" "$2"); shift 2 ;;
		-*) flags+=("$1"); shift ;;
		*) inputs+=("$1"); shift ;;
	esac; done
	((${#inputs[@]})) || { echo "linux-metal: $tool: no input files" >&2; exit 2; }
	[ -n "$out" ] || { echo "linux-metal: $tool: -o <output> is required" >&2; exit 2; }
	key=$( { printf '%s\n' "$tool" "${flags[@]}" --; for f in "${inputs[@]}"; do sha256sum < "$f"; done; } | sha256sum | cut -c1-40)
	case $tool in
		metal) case " ${flags[*]} " in *" -c "*) ext=air ;; *" -S "*) ext=ll ;; *) ext=metallib ;; esac ;;
		metallib) ext=metallib ;;
	esac
}

run() {
	parse "$@"
	hit="$CACHE/$key.$ext"
	if [ ! -f "$hit" ] && [ "${X86CODE_METAL_RECORD:-0}" = 1 ]; then
		. "$ROOT/build/mac-env.sh"
		# Mac-side names: flatten inputs into one dir (the tools see basenames only; #include of siblings is not handled)
		args=(); for f in "${flags[@]}"; do args+=("$(printf %q "$f")"); done
		for f in "${inputs[@]}"; do args+=("$(printf %q "$(basename "$f")")"); done
		mkdir -p "$CACHE"; tmp=$(mktemp "$CACHE/.rec.XXXXXX")
		tar cf - -C "$(dirname "${inputs[0]}")" $(for f in "${inputs[@]}"; do basename "$f"; done) | mac "
			d=\$(mktemp -d /tmp/x86metal.XXXXXX) && cd \$d && tar xf - &&
			xcrun -sdk iphonesimulator $tool ${args[*]} -o out.$ext >&2 && cat out.$ext; r=\$?; rm -rf \$d; exit \$r" > "$tmp" ||
			{ rm -f "$tmp"; echo "linux-metal: recording on $MAC failed" >&2; exit 1; }
		mv "$tmp" "$hit"
		{ printf '%s %s.%s %s' "$(date -u +%FT%TZ)" "$key" "$ext" "$tool"; printf ' %q' "${flags[@]}"
		  for f in "${inputs[@]}"; do printf ' %s@%s' "$(basename "$f")" "$(sha256sum < "$f" | cut -c1-16)"; done; echo; } >> "$CACHE/MANIFEST"
		echo "linux-metal: recorded $key.$ext from $MAC" >&2
	fi
	if [ ! -f "$hit" ]; then
		echo "linux-metal: no Mac-built artifact for: $tool ${flags[*]} ${inputs[*]} (key $key.$ext)." >&2
		echo "  Metal shaders cannot be compiled on Linux; the source or flags changed since the last Mac build." >&2
		echo "  Re-record on the Mac: X86CODE_METAL_RECORD=1 $0 $tool ${flags[*]} ${inputs[*]} -o $out" >&2
		exit 1
	fi
	cp "$hit" "$out"
}

me=$(basename "$0")
case $me in metal|metallib) run "$me" "$@"; exit ;; esac
cmd=${1:-}; shift || true
case $cmd in
	metal|metallib) run "$cmd" "$@" ;;
	key) parse "$@"; echo "$key.$ext" ;;
	inspect) inspect "$@" ;;
	*) sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//' >&2; exit 2 ;;
esac
