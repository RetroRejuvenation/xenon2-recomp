#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Run the patched Hatari once, headless, with the x2ref harness (patches/0001-x2ref-harness.patch).
# usage: run.sh OUTDIR QUIT_VBL [SHOTS] [INPUT_SCRIPT] [RAM_VBLS]
#   env DISK   floppy image for drive A (.st/.msa; .stx needs a Hatari built with capsimage)   [required]
#       TOS    EmuTOS image (default: EmuTOS 1.4 etos256uk.img (PAL, 50 Hz), fetched by build.sh; the us image boots NTSC at 60 Hz)
#       MEMSIZE ST RAM in KiB (default 512; Hatari reads values above 14 as KiB)
#       MACHINE st|ste (default st); JOY1=keys|none (default keys: the scripted joystick on port 1; port 0 via JOY0)
#       WAV=1  record the YM output into OUTDIR/ym.wav; use FF=off with it (fast-forward drops audio: see the note)
#       AUTO   an Atari path to run from the EmuTOS desktop instead of booting the disk (e.g. 'A:\\X2.PRG' for a cover disk)
#       FF=on|off  Hatari fast-forward (default on: about 35x real time; off = real time, needed for complete audio)
#       WATCH  ST RAM word addresses (hex, comma separated) logged every VBL into frames.log
#       X2_GENERATED (default <repo>/generated), HATARI_BIN (default: the build.sh output)
# SHOTS/RAM_VBLS syntax: "100,200-260,1000-2000/50" (/n = every n-th VBL). INPUT_SCRIPT: see src/x2ref.c in the patch
# (lines "<vbl> JOY1_FIRE 1" ...). VBL numbers count from emulator start (the first VBL is 1).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
GEN=${X2_GENERATED:-$ROOT/generated}
R="$GEN/ref/hatari"
OUT=$1
QUIT=$2
: "${DISK:?set DISK to a .st/.msa image}"
TOS=${TOS:-$R/emutos/256k/emutos-256k-1.4/etos256uk.img}
MEMSIZE=${MEMSIZE:-512}
MACHINE=${MACHINE:-st}
HAT=${HATARI_BIN:-$(find "$R/build" -iname hatari -type f -perm -u+x | head -1)}
mkdir -p "$OUT"
SOUND=off
if [ "${WAV:-}" = 1 ]; then SOUND=44100; fi
JOY0=${JOY0:-none}
JOY1=${JOY1:-keys}
# a private HOME keeps Hatari from reading or writing the user's own configuration
H="$OUT/home"; mkdir -p "$H"
: > "$OUT/hatari.cfg"
HOME="$H" X2REF_DIR="$OUT" X2REF_QUIT="$QUIT" X2REF_SHOTS="${3:-}" X2REF_INPUT="${4:-}" X2REF_RAM="${5:-}" \
X2REF_WATCH="${WATCH:-}" X2REF_WAV="${WAV:-}" \
SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} \
  "$HAT" --configfile "$OUT/hatari.cfg" --machine "$MACHINE" --memsize "$MEMSIZE" --tos "$TOS" --disk-a "$DISK" \
    --drive-b off --fast-forward ${FF:-on} --frameskips 0 --statusbar off --sound "$SOUND" --joy0 "$JOY0" --joy1 "$JOY1" \
    ${AUTO:+--auto "$AUTO"} --confirm-quit off --log-file "$OUT/hatari.log" --log-level info --screenshot-format png > "$OUT/stdout.log" 2>&1 || true
echo "frames: $(wc -l < "$OUT/frames.log" 2>/dev/null || echo 0)  shots: $(ls "$OUT"/shot_*.png 2>/dev/null | wc -l)"
