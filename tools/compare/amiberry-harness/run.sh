#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Run the patched Amiberry once, headless, with the x2ref harness.
# usage: run.sh OUTDIR QUIT_FRAME [SHOTS] [INPUT_SCRIPT] [RAM_FRAMES]
#   env KICK=... (default ~/Amiberry/Roms/x2recomp-kick13-34.5-a500.rom), DF0=..., DF1=...
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
R="$ROOT/generated/ref"
OUT=$1
QUIT=$2
KICK=${KICK:-$HOME/Amiberry/Roms/x2recomp-kick13-34.5-a500.rom}
DF0=${DF0:-"$R/adf/Xenon II - Megablast (1of2)[t+3].adf"}
DF1=${DF1:-"$R/adf/Xenon II - Megablast (2of2)[t+3].adf"}
mkdir -p "$OUT"
sed -e "s|@KICK@|$KICK|" -e "s|@DF0@|$DF0|" -e "s|@DF1@|$DF1|" "$HERE/x2ref.uae.in" > "$OUT/x2ref.uae"
BIN="$R/amiberry-build/Amiberry.app/Contents/MacOS/Amiberry"
X2REF_DIR="$OUT" X2REF_QUIT="$QUIT" X2REF_SHOTS="${3:-}" X2REF_INPUT="${4:-}" X2REF_RAM="${5:-}" X2REF_WAV=1 \
SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} \
    "$BIN" -f "$OUT/x2ref.uae" -G > "$OUT/stdout.log" 2>&1
