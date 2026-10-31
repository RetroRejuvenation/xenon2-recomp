#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Run the patched Amiberry once, headless, as a CDTV with the game's CD image (reference for the CDTV release).
# usage: run-cdtv.sh OUTDIR QUIT_FRAME [SHOTS] [INPUT_SCRIPT] [RAM_FRAMES]
#   env KICK=<Kickstart 1.3 ROM, 256 KB>  EXT=<CDTV extended ROM v1.0, v2.30 or v2.35, 256 KB>  CD=<.cue of the CDTV disc>
#   (all user-supplied; none is in the repository). Optional: X2REF_LOG=1 writes Amiberry's own log to OUTDIR/amiberry.log.
# The game starts at the 'SELECT JOYPAD MODE' screen: a script with JOY2_FIRE every ~150 frames from frame 700 reaches
# the menus, 'LOADING LEVEL 1' (about frame 3200) and level 1 (about frame 4700). The paula.wav tap includes the CD audio.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
R="${X2REF_REF:-$ROOT/generated/ref}"
OUT=$1
QUIT=$2
: "${KICK:?set KICK to a Kickstart 1.3 ROM}" "${EXT:?set EXT to the CDTV extended ROM}" "${CD:?set CD to the CDTV disc .cue}"
mkdir -p "$OUT"
sed -e "s|@KICK@|$KICK|" -e "s|@EXT@|$EXT|" -e "s|@CD@|$CD|" "$HERE/x2ref-cdtv.uae.in" > "$OUT/x2ref-cdtv.uae"
BIN="$R/amiberry-build/Amiberry.app/Contents/MacOS/Amiberry"
X2REF_DIR="$OUT" X2REF_QUIT="$QUIT" X2REF_SHOTS="${3:-}" X2REF_INPUT="${4:-}" X2REF_RAM="${5:-}" X2REF_WAV=1 \
SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} \
    "$BIN" -f "$OUT/x2ref-cdtv.uae" -G ${X2REF_LOG:+-o write_logfile=yes -o logfile_path="$OUT/amiberry.log"} > "$OUT/stdout.log" 2>&1
