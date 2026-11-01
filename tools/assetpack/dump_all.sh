#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Dump the graphics of every committed replay script of a build into ONE dump folder, so every level, boss, shop and
# the ending are covered (a single `--dump-assets` run only sees what that replay draws).
#
#   tools/assetpack/dump_all.sh -o DUMP_DIR [-g disk|cdtv|st] [-a ASSETS_DIR] [-x x2recomp] [-r REPLAY_DIR]
#                               [-f FRAMES] [-k] [-- extra x2recomp options]
#
#   -o DIR     the merged dump folder to create (required; must not exist or be empty)
#   -g GAME    disk (default), cdtv or st: picks the replay folder and passes --game
#   -a DIR     --assets for x2recomp (your own imported game; default: x2recomp's own default)
#   -x EXE     the x2recomp executable (default: $X2RECOMP, else build/x2recomp under the repository)
#   -r DIR     the replay scripts (default: tests/replays, tests/replays/cdtv or tests/replays/st)
#   -f FRAMES  frames to run per replay (default 6000, the length of the committed scripts)
#   -k         keep the per-replay dumps and logs in DUMP_DIR/.parts after merging
#
# The dump holds your own copy's graphics: for making a pack, never for distributing. Needs Python 3 and a built
# x2recomp. Environment: X2RECOMP, PYTHON. Nothing here is specific to a machine: all paths are arguments.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
GAME=disk
OUT=
ASSETS=
EXE=${X2RECOMP:-$ROOT/build/x2recomp}
REPLAYS=
FRAMES=6000
KEEP=0
PYTHON=${PYTHON:-python3}

usage() { sed -n '4,19p' "$0" | sed 's/^# \{0,1\}//'; }

while getopts "o:g:a:x:r:f:kh" opt; do
  case $opt in
    o) OUT=$OPTARG ;;
    g) GAME=$OPTARG ;;
    a) ASSETS=$OPTARG ;;
    x) EXE=$OPTARG ;;
    r) REPLAYS=$OPTARG ;;
    f) FRAMES=$OPTARG ;;
    k) KEEP=1 ;;
    h) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done
shift $((OPTIND - 1))
if [ "${1:-}" = "--" ]; then shift; fi

[ -n "$OUT" ] || { echo "dump_all: -o DUMP_DIR is required" >&2; usage >&2; exit 2; }
case $GAME in
  disk) [ -n "$REPLAYS" ] || REPLAYS=$ROOT/tests/replays ;;
  cdtv) [ -n "$REPLAYS" ] || REPLAYS=$ROOT/tests/replays/cdtv ;;
  st)   [ -n "$REPLAYS" ] || REPLAYS=$ROOT/tests/replays/st ;;
  *) echo "dump_all: -g must be disk, cdtv or st" >&2; exit 2 ;;
esac
[ -x "$EXE" ] || { echo "dump_all: x2recomp not found or not executable: $EXE (build it, or pass -x)" >&2; exit 1; }
[ -d "$REPLAYS" ] || { echo "dump_all: replay folder not found: $REPLAYS" >&2; exit 1; }
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then
  echo "dump_all: $OUT exists and is not empty" >&2
  exit 1
fi

# The per-replay dumps live beside OUT while they are made, so OUT stays absent until the merge succeeds.
PARTS=$OUT.parts.$$
mkdir -p "$PARTS"
n=0
failed=0
for script in "$REPLAYS"/*.txt; do
  [ -f "$script" ] || continue
  name=$(basename "$script" .txt)
  part=$PARTS/$name
  echo "dump_all: $name" >&2
  if "$EXE" --headless --report-every 0 --game "$GAME" ${ASSETS:+--assets "$ASSETS"} \
        --input-script "$script" --frames "$FRAMES" --dump-assets "$part" "$@" > "$PARTS/$name.log" 2>&1; then
    if [ -f "$part/manifest.json" ]; then n=$((n + 1)); fi
  else
    echo "dump_all: $name failed (log kept in $PARTS/$name.log); skipped" >&2
    failed=$((failed + 1))
    rm -rf "$part"
  fi
done

if [ "$n" -eq 0 ]; then
  echo "dump_all: no replay produced a dump (logs in $PARTS)" >&2
  exit 1
fi
list=
for d in "$PARTS"/*/; do
  if [ -f "${d}manifest.json" ]; then list="$list $d"; fi
done
rm -rf "$OUT"
# shellcheck disable=SC2086
"$PYTHON" -I "$HERE/merge_dumps.py" --out "$OUT" $list
if [ "$KEEP" = 1 ]; then
  mv "$PARTS" "$OUT/.parts"
else
  rm -rf "$PARTS"
fi
echo "dump_all: $n replays merged into $OUT ($failed skipped)" >&2
