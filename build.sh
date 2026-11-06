#!/usr/bin/env bash
# Build nicochuniio (fitouch2.dll replacement) with the MinGW-w64 i686
# cross compiler.  Requires: gcc-mingw-w64-i686 (Debian/Ubuntu) or an
# equivalent i686-w64-mingw32-gcc.
#
#   ./build.sh            -> out/fitouch2-air-polling.dll
#                            out/fitouch2-air-blocking.dll
#                            out/fitouch2.dll (same as AIR blocking)
#
set -euo pipefail

CC="${CC:-i686-w64-mingw32-gcc}"
OUT=out
mkdir -p "$OUT"

CFLAGS="-O2 -Wall -Wno-unused-function -DUNICODE -D_UNICODE -Isrc"
# note: -m32 is implicit for the i686 mingw target; kept for clarity when
# building with a multi-target toolchain.

SOURCES=(
    src/core/chuniio.c
    src/core/config.c
    src/core/input_packet.c
    src/feat/cursor.c
    src/hardware/io4_port.c
    src/hardware/serial_port.c
    src/protocol/packet_solver.c
    src/protocol/slider_protocol.c
    src/utils/queue.c
    src/utils/log.c
    src/dllmain.c
)

build_variant() {
    local output="$1"
    shift
    "$CC" $CFLAGS "$@" -shared -o "$OUT/$output" \
        -Wl,--kill-at \
        "${SOURCES[@]}" \
        nicochuniio.def \
        -lsetupapi -lhid -lwinmm \
        -static-libgcc
    echo "built: $OUT/$output"
}

build_variant fitouch2-air-polling.dll
build_variant fitouch2-air-blocking.dll -DAIR_BLOCKING_READ
cp "$OUT/fitouch2-air-blocking.dll" "$OUT/fitouch2.dll"
echo "built: $OUT/fitouch2.dll (AIR blocking default)"
