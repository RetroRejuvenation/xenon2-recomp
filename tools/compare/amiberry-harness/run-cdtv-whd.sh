#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Run the patched Amiberry once, headless, with the x2ref harness, as an A500+ (ECS Agnus, 1 MB chip, 1 MB fast) that
# boots a host directory containing WHDLoad and the CDTV install of Xenon 2 (Xenon2CDTV.Slave, v1.6 by Wepl), prepared
# by prep-cdtv-whd.sh.
# usage: run-cdtv-whd.sh OUTDIR QUIT_FRAME [SHOTS] [INPUT_SCRIPT] [RAM_FRAMES]
#   env KICK=<Kickstart 2.04 ROM, user-supplied: the current WHDLoad does not load under 1.3>
#       DH0=<host directory prepared by prep-cdtv-whd.sh>   X2REF_REF=<directory holding amiberry-build/>
#   (optional) X2REF_LOG=1 writes Amiberry's log to OUTDIR/amiberry.log.
# The harness knobs of run.sh apply unchanged (X2REF_SHOTS, X2REF_RAM, X2REF_WAV, X2REF_PCTRACE, ...); frame numbers are
# vsync counts since the hard reset, so the game starts several hundred frames in (DOS boot + WHDLoad start).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
R="${X2REF_REF:-$ROOT/generated/ref}"
OUT=$1
QUIT=$2
: "${KICK:?set KICK to a Kickstart 2.04 ROM}" "${DH0:?set DH0 to the prepared install directory}"
mkdir -p "$OUT"
sed -e "s|@KICK@|$KICK|" -e "s|@DH0@|$DH0|" "$HERE/x2ref-cdtv-whd.uae.in" > "$OUT/x2ref-cdtv-whd.uae"
BIN="$R/amiberry-build/Amiberry.app/Contents/MacOS/Amiberry"
X2REF_DIR="$OUT" X2REF_QUIT="$QUIT" X2REF_SHOTS="${3:-}" X2REF_INPUT="${4:-}" X2REF_RAM="${5:-}" X2REF_WAV=1 \
SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} \
    "$BIN" -f "$OUT/x2ref-cdtv-whd.uae" -G ${X2REF_LOG:+-o write_logfile=yes -o logfile_path="$OUT/amiberry.log"} > "$OUT/stdout.log" 2>&1
