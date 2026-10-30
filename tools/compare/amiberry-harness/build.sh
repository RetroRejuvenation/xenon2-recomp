#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Build a patched Amiberry v7.0.8 with the x2recomp reference harness (x2ref_harness.inc).
# Everything goes under generated/ref (gitignored); nothing is installed system-wide.
# Needs: git, cmake, ninja, Homebrew SDL2 headers (sdl2-compat), flac, mpg123, libpng, and an installed
# /Applications/Amiberry.app. Its bundled dylibs (SDL2, SDL2_image, SDL2_ttf and their dependencies,
# all with @executable_path/../Frameworks install names) are COPIED: SDL2/image/ttf into deps/lib to link
# against, and the whole Frameworks folder into the built bundle so exactly one SDL2 is loaded at run time.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
R="$ROOT/generated/ref"
SRC="$R/amiberry-src"
BLD="$R/amiberry-build"
APP=/Applications/Amiberry.app/Contents/Frameworks

if [ ! -d "$SRC" ]; then
    git clone --depth 1 --branch v7.0.8 https://github.com/BlitterStudio/amiberry.git "$SRC"
fi
cd "$SRC"
# (Re)apply the patch only when the tree does not already match it, so a re-run resumes the build
# without touching up-to-date objects.
if ! git apply --reverse --check "$HERE/x2ref-hooks.patch" 2>/dev/null; then
    git diff --name-only | xargs git checkout --
    git apply "$HERE/x2ref-hooks.patch"
fi
cmp -s "$HERE/x2ref_harness.inc" src/osdep/x2ref_harness.inc || cp "$HERE/x2ref_harness.inc" src/osdep/x2ref_harness.inc

mkdir -p "$R/deps/include/SDL2" "$R/deps/lib"
[ -f "$R/deps/include/SDL2/SDL_image.h" ] || curl -sSfL -o "$R/deps/include/SDL2/SDL_image.h" \
    https://raw.githubusercontent.com/libsdl-org/SDL_image/release-2.8.8/include/SDL_image.h
[ -f "$R/deps/include/SDL2/SDL_ttf.h" ] || curl -sSfL -o "$R/deps/include/SDL2/SDL_ttf.h" \
    https://raw.githubusercontent.com/libsdl-org/SDL_ttf/release-2.24.0/SDL_ttf.h
for f in SDL2 SDL2_image SDL2_ttf; do
    [ -f "$R/deps/lib/lib$f-2.0.0.dylib" ] || cp "$APP/lib$f-2.0.0.dylib" "$R/deps/lib/"
    ln -sf "lib$f-2.0.0.dylib" "$R/deps/lib/lib$f.dylib"
done

cmake -S "$SRC" -B "$BLD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DUSE_LIBSERIALPORT=OFF -DUSE_LIBENET=OFF -DUSE_PORTMIDI=OFF -DUSE_LIBMPEG2=OFF \
    -DSDL2_IMAGE_INCLUDE_DIR="$R/deps/include/SDL2" -DSDL2_IMAGE_LIBRARY="$R/deps/lib/libSDL2_image-2.0.0.dylib" \
    -DSDL2_TTF_INCLUDE_DIR="$R/deps/include/SDL2" -DSDL2_TTF_LIBRARY="$R/deps/lib/libSDL2_ttf-2.0.0.dylib" \
    -DCMAKE_PREFIX_PATH=/opt/homebrew -DX2REF_LINK_FLAGS="-L$R/deps/lib"
mkdir -p "$BLD/Amiberry.app/Contents/Frameworks"
cp -n "$APP"/*.dylib "$BLD/Amiberry.app/Contents/Frameworks/" 2>/dev/null || true
# -k 0: keep going so one run reports every compile error, not just the first
cmake --build "$BLD" -- -k 0
