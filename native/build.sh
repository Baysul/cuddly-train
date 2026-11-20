#!/bin/sh
# Builds xivperf_native.dll (Windows x64) with mingw-w64: build.sh [output dir, default ./bin]
# It links kernel32 and the UCRT (api-ms-win-crt-*), both in Wine. Keeps COFF symbols; the analyzer names frames by export.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/bin}
mkdir -p "$out"
x86_64-w64-mingw32-gcc -std=gnu11 -O2 -Wall -Wextra -Werror -shared -static-libgcc -Wl,--no-insert-timestamp -Wl,--disable-auto-image-base \
    -o "$out/xivperf_native.dll.tmp" "$here/xivperf_native.c"
mv -f "$out/xivperf_native.dll.tmp" "$out/xivperf_native.dll"   # a new file, never a rewrite of a loaded one
