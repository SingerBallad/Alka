#!/bin/bash
# The upstream V8 headers a Node release leaves out (the Fast API, the
# inspector, libplatform's extras), for V8 <tag>, copied into <dest> --
# nothing <node-include> has is shadowed.
#   fetch-v8-headers.sh <v8-tag e.g. 12.4.254.21> <node-include-dir> <dest>
set -euo pipefail
tag="$1"; node_include="$2"; dest="$3"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
curl -fsSL --retry 3 -o "$work/include.tar.gz" \
    "https://chromium.googlesource.com/v8/v8/+archive/refs/tags/$tag/include.tar.gz"
mkdir -p "$work/include" "$dest"
tar -xzf "$work/include.tar.gz" -C "$work/include"
for header in "$work"/include/*.h; do
    name=$(basename "$header")
    [ -e "$node_include/$name" ] || cp "$header" "$dest/"
done
for dir in libplatform; do
    [ -e "$node_include/$dir" ] || cp -r "$work/include/$dir" "$dest/"
done
