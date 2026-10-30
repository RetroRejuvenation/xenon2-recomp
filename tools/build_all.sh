#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# One pipeline for end users: (a) build x2recomp, (b) import your own Xenon 2 disk images, (c) optionally
# generate the statically recompiled code from the imported image and build the recompiled executable.
#
#   tools/build_all.sh [--recomp] [--test] [--build-dir DIR] [--data-dir DIR] [DISK_IMAGE_OR_ZIP_OR_FOLDER...]
#
#   (a) always: cmake configure + build into DIR (default build/)
#   (b) with images given: DIR/x2recomp --import ...  (into the per-user data directory, or --data-dir)
#   (c) --recomp: needs Python 3. Replays tests/replays/*.txt on the imported game to record coverage traces,
#       runs tools/recomp/recomp.py on the imported image with them and builds DIR-recomp with -DX2_RECOMP=ON.
#       The translated code is derived from your game data: it stays in DIR (never commit or share it).
#   --test: run ctest on the builds made.
# docs/BUILDING.md describes each step. Windows: tools/build_all.ps1 (same steps and options).
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/build"
DATA=""
RECOMP=0
TEST=0
IMAGES=""
while [ $# -gt 0 ]; do
  case "$1" in
    --recomp) RECOMP=1 ;;
    --test) TEST=1 ;;
    --build-dir) BUILD=$(cd "$(dirname "$2")" && pwd)/$(basename "$2"); shift ;;
    --data-dir) DATA="$2"; shift ;;
    -h|--help) sed -n '4,15p' "$0"; exit 0 ;;
    *) IMAGES="$IMAGES
$1" ;;
  esac
  shift
done

PY=python3
if ! command -v python3 > /dev/null 2>&1 && command -v python > /dev/null 2>&1; then PY=python; fi
GEN=""
if command -v ninja > /dev/null 2>&1; then GEN="-G Ninja"; fi
JOBS=$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 4)

echo "== (a) build: $BUILD"
# shellcheck disable=SC2086
cmake -S "$ROOT" -B "$BUILD" $GEN -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$BUILD" -j "$JOBS"

DATA_ARG=""
if [ -n "$DATA" ]; then DATA_ARG="--data-dir $DATA"; fi
if [ -n "$IMAGES" ]; then
  echo "== (b) import"
  # One argument per line in $IMAGES (paths may contain spaces and brackets).
  OLDIFS=$IFS
  IFS='
'
  # shellcheck disable=SC2086
  set -- $IMAGES
  IFS=$OLDIFS
  # shellcheck disable=SC2086
  if ! "$BUILD/x2recomp" $DATA_ARG --import "$@"; then
    echo "import failed: see the messages above (exit code 2 = bad or missing disc, 3 = unsupported release/format)" >&2
    exit 1
  fi
fi

# Where the imported assets are: x2recomp itself knows its per-user data directory (SDL_GetPrefPath).
if [ -n "$DATA" ]; then
  ASSETS="$DATA/assets"
else
  ASSETS="$("$BUILD/x2recomp" --print-data-dir || true)assets"
fi

if [ "$RECOMP" = 1 ]; then
  echo "== (c) recompiled build from $ASSETS"
  if [ ! -f "$ASSETS/manifest.txt" ]; then
    echo "no imported game data in $ASSETS: give your disk images to this script first" >&2
    exit 1
  fi
  "$PY" -I "$ROOT/tools/trace/run_corpus.py" --exe "$BUILD/x2recomp" --assets "$ASSETS" --out "$BUILD/x2-trace"
  "$PY" -I "$ROOT/tools/recomp/recomp.py" --no-lane --assets "$ASSETS" --trace "$BUILD/x2-trace" \
    --out "$BUILD/x2-recomp-src"
  # shellcheck disable=SC2086
  cmake -S "$ROOT" -B "$BUILD-recomp" $GEN -DCMAKE_BUILD_TYPE=Release -DX2_RECOMP=ON \
    -DX2_RECOMP_DIR="$BUILD/x2-recomp-src" -DX2_ASSETS_DIR="$ASSETS" > /dev/null
  cmake --build "$BUILD-recomp" -j "$JOBS"
fi

if [ "$TEST" = 1 ]; then
  echo "== tests"
  # Game-data tests use X2_ASSETS_DIR (default generated/assets); point them at the import if that is absent.
  if [ ! -f "$ROOT/generated/assets/game_0400.bin" ] && [ -f "$ASSETS/game_0400.bin" ]; then
    cmake -S "$ROOT" -B "$BUILD" -DX2_ASSETS_DIR="$ASSETS" > /dev/null
  fi
  ctest --test-dir "$BUILD" -j "$JOBS" --output-on-failure
  if [ "$RECOMP" = 1 ]; then ctest --test-dir "$BUILD-recomp" -j "$JOBS" --output-on-failure; fi
fi

echo "== done"
echo "run: $BUILD/x2recomp${DATA:+ --data-dir $DATA}"
if [ "$RECOMP" = 1 ]; then echo "recompiled: $BUILD-recomp/x2recomp${DATA:+ --data-dir $DATA}"; fi
