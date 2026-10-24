#!/bin/bash
# Builds tests/Node as an addon for an Electron release (Linux x64) and runs
# test.js inside it (ELECTRON_RUN_AS_NODE) -- the recipe any Electron addon
# built on Alka follows:
#   * V8's C++ API passes std types, and Electron's are those of its own
#     libc++ (namespace std::__Cr): compile against the release's libc++
#     headers and link its libc++ objects. Those are ThinLTO bitcode of
#     Chromium's clang, so the addon is built with that clang (and its lld).
#   * The V8 ABI macros the release's config.gypi implies (pointer
#     compression, the sandbox).
#   * ALKA_HAS_CPPGC=0: Electron exports no cppgc allocation internals, so
#     Alka::GcObject classes are not available in its addons.
#
#   ci/electron-addon.sh <electron-version> <work-dir>    e.g. 45.0.0 /tmp/electron
set -euo pipefail
ver="$1"
work="$(mkdir -p "$2" && cd "$2" && pwd)"
repo="$(cd "$(dirname "$0")/.." && pwd)"
dl="$work/dl"
mkdir -p "$dl"

fetch() {  # url file
    [ -f "$dl/$2" ] || curl -fsSL --retry 3 -o "$dl/$2" "$1"
}
release="https://github.com/electron/electron/releases/download/v$ver"
artifacts="https://artifacts.electronjs.org/headers/dist/v$ver"
for f in "electron-v$ver-linux-x64.zip" libcxx_headers.zip libcxxabi_headers.zip \
         "libcxx-objects-v$ver-linux-x64.zip"; do
    fetch "$release/$f" "$f"
done
fetch "$release/SHASUMS256.txt" SHASUMS256.txt
fetch "$artifacts/node-v$ver-headers.tar.gz" "node-v$ver-headers.tar.gz"
fetch "$artifacts/SHASUMS256.txt" SHASUMS256-headers.txt
(cd "$dl" && grep -E " \*?(electron-v$ver-linux-x64|libcxx_headers|libcxxabi_headers|libcxx-objects-v$ver-linux-x64)\.zip$" \
     SHASUMS256.txt | sed 's/ \*/  /' | sha256sum -c --quiet -)
(cd "$dl" && grep -E " node-v$ver-headers\.tar\.gz$" SHASUMS256-headers.txt | sha256sum -c --quiet -)

unpack() {  # archive dir
    [ -d "$work/$2" ] || { mkdir -p "$work/$2" && python3 -m zipfile -e "$dl/$1" "$work/$2"; }
}
unpack "electron-v$ver-linux-x64.zip" electron
unpack libcxx_headers.zip libcxx
unpack libcxxabi_headers.zip libcxxabi
unpack "libcxx-objects-v$ver-linux-x64.zip" libcxx-objects
chmod +x "$work/electron/electron" "$work/electron/chrome_crashpad_handler"
[ -d "$work/headers" ] || { mkdir -p "$work/headers" && tar -xzf "$dl/node-v$ver-headers.tar.gz" -C "$work/headers"; }
headers="$work/headers/node_headers"
electron="$work/electron/electron"

versions() { ELECTRON_RUN_AS_NODE=1 "$electron" -p "process.versions.$1"; }
chrome="$(versions chrome)"
v8tag="$(versions v8 | sed 's/-electron.*//')"
echo "Electron $ver: Chromium $chrome, V8 $v8tag"

# Chromium's clang of that release (tools/clang/scripts/update.py names it)
update="$(curl -fsSL --retry 3 "https://chromium.googlesource.com/chromium/src/+/refs/tags/$chrome/tools/clang/scripts/update.py?format=TEXT" | base64 -d)"
revision="$(sed -nE "s/^CLANG_REVISION = '([^']+)'.*/\1/p" <<<"$update")"
sub="$(sed -nE 's/^CLANG_SUB_REVISION = ([0-9]+).*/\1/p' <<<"$update")"
clang="$work/clang-$revision-$sub"
if [ ! -x "$clang/bin/clang++" ]; then
    fetch "https://commondatastorage.googleapis.com/chromium-browser-clang/Linux_x64/clang-$revision-$sub.tar.xz" \
        "clang-$revision-$sub.tar.xz"
    mkdir -p "$clang" && tar -xJf "$dl/clang-$revision-$sub.tar.xz" -C "$clang"
fi

# the libc++ objects as indexed archives (the release ships thin ones without)
for lib in libc++ libc++abi; do
    [ -f "$work/$lib-lto.a" ] || "$clang/bin/llvm-ar" rcs "$work/$lib-lto.a" "$work/libcxx-objects/$lib"/*.o
done

# the upstream V8 headers Electron's leave out (the Fast API, the inspector)
[ -d "$work/v8-extra" ] || bash "$repo/ci/fetch-v8-headers.sh" "$v8tag" "$headers/include/node" "$work/v8-extra"

# V8's ABI macros, as node's common.gypi derives them from config.gypi
abi="$(python3 - "$headers/include/node/config.gypi" <<'PY'
import ast, re, sys
text = re.sub(r"#.*", "", open(sys.argv[1]).read())
v = ast.literal_eval(text)["variables"]
on = lambda key: str(v.get(key, 0)) in ("1", "true")
defines = []
if on("v8_enable_pointer_compression"):
    defines.append("V8_COMPRESS_POINTERS")
    defines.append("V8_COMPRESS_POINTERS_IN_SHARED_CAGE" if on("v8_enable_pointer_compression_shared_cage")
                   else "V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES")
if on("v8_enable_pointer_compression") or on("v8_enable_31bit_smis_on_64bit_arch"):
    defines.append("V8_31BIT_SMIS_ON_64BIT_ARCH")
for key, define in [("v8_enable_sandbox", "V8_ENABLE_SANDBOX"),
                    ("v8_enable_external_code_space", "V8_EXTERNAL_CODE_SPACE"),
                    ("v8_enable_webassembly", "V8_ENABLE_WEBASSEMBLY"),
                    ("v8_enable_zone_compression", "V8_COMPRESS_ZONES"),
                    ("v8_enable_v8_checks", "V8_ENABLE_CHECKS")]:
    if on(key):
        defines.append(define)
print(" ".join("-D" + d for d in defines))
PY
)"
echo "V8 ABI: $abi"

# Electron's libc++: its headers (and what Chromium's build passes besides
# __config_site), its objects linked in
libcxx="-nostdinc++ -isystem $work/libcxx -isystem $work/libcxx/include -isystem $work/libcxxabi/include"
libcxx+=" -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE -D_LIBCPP_INSTRUMENTED_WITH_ASAN=0"
build="$work/build"
cmake -S "$repo/tests/Node" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER="$clang/bin/clang++" \
    -DNODE_DIR="$headers" -DNODE_EXTRA_INCLUDE="$work/v8-extra" \
    "-DCMAKE_CXX_FLAGS=-DALKA_HAS_CPPGC=0 $abi $libcxx" \
    "-DCMAKE_MODULE_LINKER_FLAGS=-nostdlib++ -fuse-ld=lld" \
    "-DCMAKE_CXX_STANDARD_LIBRARIES=$work/libc++-lto.a $work/libc++abi-lto.a"
cmake --build "$build"

cd "$build"
ELECTRON_RUN_AS_NODE=1 ALKA_ADDON="$build/alka_node_test.node" \
    "$electron" --expose-gc --allow-natives-syntax "$repo/tests/Node/test.js"
