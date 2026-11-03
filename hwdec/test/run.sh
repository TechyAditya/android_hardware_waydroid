#!/usr/bin/env bash
# Builds rgbatest and runs it on CLIP against private copies of waydroid-alloc and waydroid-hwdec
# from ../../build/host (run ../../build.sh first).  Needs no Waydroid session and no root.
# usage: run.sh CLIP; rgbatest.c lists the environment variables it takes.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
bin=${HOST_BIN:-$here/../../build/host/bin}
work=$(mktemp -d)
trap 'kill $(jobs -p) 2> /dev/null || true; rm -rf "$work"' EXIT

cc -O2 -Wall -o "$work/rgbatest" "$here/rgbatest.c" \
   $(pkg-config --cflags --libs libdrm libavformat libavcodec libavutil libswscale vulkan)

"$bin/waydroid-alloc" --socket-path "$work/alloc.sock" --allowed-uid "$(id -u)" > "$work/alloc.log" 2>&1 &
"$bin/waydroid-hwdec" --socket-path "$work/hwdec.sock" --allowed-uid "$(id -u)" > "$work/hwdec.log" 2>&1 &
for _ in $(seq 50); do
   [ -S "$work/alloc.sock" ] && [ -S "$work/hwdec.sock" ] && break
   sleep 0.1
done
"$work/rgbatest" "$work/hwdec.sock" "$work/alloc.sock" "$1" || { cat "$work"/*.log >&2; exit 1; }
