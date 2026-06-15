#!/bin/bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

set -e

SRC="$1"
OUT="$2"
WORKDIR="$3"

if [ -z "$SRC" ] || [ -z "$OUT" ] || [ -z "$WORKDIR" ]; then
    echo "Usage: $0 <source.png> <output.icns> <work_dir>" >&2
    exit 1
fi

ICONSET="$WORKDIR/$(basename "${OUT%.icns}").iconset"

rm -rf "$ICONSET"
mkdir -p "$ICONSET"

for ICSIZE in 16 32 128 256 512; do
    sips -z "$ICSIZE" "$ICSIZE" "$SRC" --out "$ICONSET/icon_${ICSIZE}x${ICSIZE}.png" >/dev/null
    sips -z $((ICSIZE*2)) $((ICSIZE*2)) "$SRC" --out "$ICONSET/icon_${ICSIZE}x${ICSIZE}@2x.png" >/dev/null
done

iconutil -c icns "$ICONSET" -o "$OUT"

rm -rf "$ICONSET"
