#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Build a local, uncommitted Hatari + EmuTOS reference under generated/ref/hatari/ (nothing is installed system-wide).
# Hatari (GPL-2.0-or-later) and EmuTOS (GPL-2.0-or-later with exceptions) are only used as a reference tool: nothing
# from them is shipped, and no TOS/ROM from Atari is needed (EmuTOS replaces it).
#
#   tools/compare/hatari-harness/build.sh
#
# env: X2_GENERATED (default <repo>/generated; use it from a git worktree), HATARI_VERSION (default 2.6.1), EMUTOS_VERSION (default 1.4), SDL2_PREFIX (a prefix holding SDL2: default: ask
#      sdl2-config; set it if SDL2 is not on the PATH), JOBS, WITH_CAPSIMAGE=1 (only if you built capsimg yourself:
#      CAPSIMAGE_PREFIX), X2REF_NO_PATCH=1 (build stock Hatari without the harness patch).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
GEN=${X2_GENERATED:-$ROOT/generated}
R="$GEN/ref/hatari"
HATARI_VERSION=${HATARI_VERSION:-2.6.1}
EMUTOS_VERSION=${EMUTOS_VERSION:-1.4}
JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}
mkdir -p "$R/dl" "$R/src" "$R/emutos"

fetch() { # url file
  [ -s "$R/dl/$2" ] || curl -fL --max-time 600 -o "$R/dl/$2" "$1"
}
fetch "https://github.com/hatari/hatari/archive/refs/tags/v$HATARI_VERSION.tar.gz" "hatari-$HATARI_VERSION.tar.gz"
fetch "https://sourceforge.net/projects/emutos/files/emutos/$EMUTOS_VERSION/emutos-512k-$EMUTOS_VERSION.zip/download" "emutos-512k-$EMUTOS_VERSION.zip"
fetch "https://sourceforge.net/projects/emutos/files/emutos/$EMUTOS_VERSION/emutos-256k-$EMUTOS_VERSION.zip/download" "emutos-256k-$EMUTOS_VERSION.zip"
( cd "$R/dl" && shasum -a 256 hatari-*.tar.gz emutos-*.zip | tee SHA256SUMS.txt )

SRC="$R/src/hatari-$HATARI_VERSION"
if [ ! -d "$SRC" ]; then tar xzf "$R/dl/hatari-$HATARI_VERSION.tar.gz" -C "$R/src"; fi
[ -d "$R/emutos/512k" ] || unzip -oq "$R/dl/emutos-512k-$EMUTOS_VERSION.zip" -d "$R/emutos/512k"
[ -d "$R/emutos/256k" ] || unzip -oq "$R/dl/emutos-256k-$EMUTOS_VERSION.zip" -d "$R/emutos/256k"

if [ -z "${X2REF_NO_PATCH:-}" ]; then
  for p in "$HERE"/patches/*.patch; do
    [ -f "$p" ] || continue
    # a patch that still applies forward has not been applied yet (-N makes patch refuse already applied hunks)
    if (cd "$SRC" && patch -p1 -N --dry-run -s < "$p" > /dev/null 2>&1); then
      (cd "$SRC" && patch -p1 -N < "$p")
    else
      echo "patch already applied (or does not apply): $(basename "$p")"
    fi
  done
fi

PREFIX_ARG=""
if [ -n "${SDL2_PREFIX:-}" ]; then PREFIX_ARG="-DCMAKE_PREFIX_PATH=$SDL2_PREFIX"; fi
CAPS_ARG=""
[ -n "${WITH_CAPSIMAGE:-}" ] && CAPS_ARG="-DCAPSIMAGE_INCLUDE_DIR=${CAPSIMAGE_PREFIX:?}/include -DCAPSIMAGE_LIBRARY=${CAPSIMAGE_PREFIX}/lib/libcapsimage.dylib"
ARCH_ARG=""
[ "$(uname -s)" = Darwin ] && ARCH_ARG="-DCMAKE_OSX_ARCHITECTURES=$(uname -m)"   # Hatari defaults to a universal build; the libraries are usually single-arch
cmake -S "$SRC" -B "$R/build" -DCMAKE_BUILD_TYPE=Release $PREFIX_ARG $CAPS_ARG $ARCH_ARG -DENABLE_SDL2=1 > "$R/cmake.log" 2>&1 || { tail -30 "$R/cmake.log"; exit 1; }
cmake --build "$R/build" -j "$JOBS" > "$R/build.log" 2>&1 || { tail -40 "$R/build.log"; exit 1; }
HAT=$(find "$R/build" -iname hatari -type f -perm -u+x | head -1)
echo "built: $HAT"
"$HAT" --version
