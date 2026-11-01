#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Release packages for macOS and Linux from an existing build (Windows: tools/package.ps1). docs/RELEASING.md.
#
#   tools/package.sh [--build-dir DIR] [--out DIR] [--lipo OTHER_BUILD_DIR] [--sign IDENTITY] [--dmg]
#
#   --build-dir DIR   configured and built tree (default build/); must be an interpreter-only build: a -DX2_RECOMP=ON
#                     build links code translated from your game data and is refused
#   --out DIR         where the package goes (default dist/, gitignored)
#   --lipo DIR        macOS: a second build of the other architecture; x2recomp and SDL3 are merged with lipo
#                     into universal binaries (or build once with the macos-universal preset instead)
#   --sign IDENTITY   macOS: codesign identity (default "-": ad-hoc, enough to run locally; not notarised)
#   --dmg             macOS: also make a .dmg with hdiutil
#
# macOS: x2recomp.app (Contents/MacOS/x2recomp, SDL3 in Contents/Frameworks, rpath fixed, generated icon) in a
# .zip; Linux: a .tar.gz with bin/x2recomp, lib/libSDL3.so.0 (rpath $ORIGIN/../lib) and share/doc. Both carry
# LICENSE (GPL-3.0-or-later), README.md, THIRD-PARTY-NOTICES.md, docs/PROVENANCE.md, docs/BUILDING.md and the
# third-party licence texts, plus
# SHA256SUMS.txt of the contents and a .sha256 file of the archive. Packages never contain game data, imported
# assets, generated/translated code or ROMs (checked below).
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/build"
OUT="$ROOT/dist"
LIPO=""
SIGN="-"
DMG=0
while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) BUILD=$(cd "$2" && pwd); shift ;;
    --out) mkdir -p "$2"; OUT=$(cd "$2" && pwd); shift ;;
    --lipo) LIPO=$(cd "$2" && pwd); shift ;;
    --sign) SIGN="$2"; shift ;;
    --dmg) DMG=1 ;;
    -h|--help) sed -n '4,20p' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

cache_value() { sed -n "s/^$2:[A-Z]*=//p" "$1/CMakeCache.txt"; }
[ -f "$BUILD/CMakeCache.txt" ] || { echo "$BUILD is not a configured build directory" >&2; exit 1; }
for b in "$BUILD" $LIPO; do
  case "$(cache_value "$b" X2_RECOMP)" in
    ON|1|TRUE|YES|on|true)
      echo "$b is a -DX2_RECOMP=ON build: it contains code translated from game data and must not be packaged" >&2
      exit 1 ;;
  esac
done
VERSION=$(cache_value "$BUILD" CMAKE_PROJECT_VERSION)
OS=$(uname -s)
ARCH=$(uname -m)

sha256() { if command -v sha256sum > /dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }

# Stage with the project's install rules (executable, SDL3, docs, licences).
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/x2pkg.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
cmake --build "$BUILD" --config Release > /dev/null
cmake --install "$BUILD" --config Release --prefix "$STAGE/install" > /dev/null
if [ -n "$LIPO" ]; then
  cmake --build "$LIPO" --config Release > /dev/null
  cmake --install "$LIPO" --config Release --prefix "$STAGE/install-other" > /dev/null
fi

# Nothing derived from game data, no disk/CD images, audio dumps or translated code may be in a package.
check_contents() {
  bad=$(find "$1" -type f \( -iname '*.adf' -o -iname '*.adz' -o -iname '*.dms' -o -iname '*.ipf' -o -iname '*.zip' \
    -o -iname '*.iso' -o -iname '*.bin' -o -iname '*.cue' -o -iname '*.rom' -o -iname '*.wav' -o -iname '*.pcm' \
    -o -iname '*.ppm' -o -iname 'x2r_*' -o -iname 'x2rc_*' -o -iname 'x2rs_*' -o -iname 'manifest.txt' -o -iname 'settings.cfg' -o -iname 'input.cfg' \) -print)
  if [ -n "$bad" ] || [ -d "$1/generated" ]; then
    echo "package would contain forbidden files:" >&2
    echo "$bad" >&2
    exit 1
  fi
}

write_manifest() { # SHA256SUMS.txt of every file below $1, relative paths, sorted
  (cd "$1" && find . -type f ! -name SHA256SUMS.txt | LC_ALL=C sort | sed 's|^\./||' | while IFS= read -r f; do
    sha256 "$f"
  done) > "$1/SHA256SUMS.txt"
}

mkdir -p "$OUT"
if [ "$OS" = Darwin ]; then
  I="$STAGE/install"
  ARCHS=$(lipo -archs "$I/bin/x2recomp")
  if [ -n "$LIPO" ]; then
    lipo -create "$I/bin/x2recomp" "$STAGE/install-other/bin/x2recomp" -output "$STAGE/x2recomp.universal"
    mv "$STAGE/x2recomp.universal" "$I/bin/x2recomp"
    ARCHS=$(lipo -archs "$I/bin/x2recomp")
  fi
  case "$ARCHS" in
    *arm64*x86_64*|*x86_64*arm64*) TAG=universal ;;
    *) TAG=$ARCHS ;;
  esac
  NAME="x2recomp-$VERSION-macos-$TAG"
  PKG="$STAGE/$NAME"
  APP="$PKG/x2recomp.app"
  mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"
  cp "$I/bin/x2recomp" "$APP/Contents/MacOS/x2recomp"
  EXE="$APP/Contents/MacOS/x2recomp"

  # SDL3: the dylib the executable links (installed next to it, or Homebrew's), as @rpath/libSDL3.0.dylib.
  SDL_REF=$(otool -L "$EXE" | awk '/libSDL3/ {print $1; exit}')
  if [ -n "$SDL_REF" ]; then
    SDL_BASE=$(basename "$SDL_REF")
    if [ -f "$I/lib/$SDL_BASE" ]; then SDL_SRC="$I/lib/$SDL_BASE"; else SDL_SRC="$SDL_REF"; fi
    [ -f "$SDL_SRC" ] || { echo "cannot find $SDL_REF to bundle" >&2; exit 1; }
    cp -L "$SDL_SRC" "$APP/Contents/Frameworks/$SDL_BASE"
    chmod u+w "$APP/Contents/Frameworks/$SDL_BASE"
    if [ -n "$LIPO" ]; then
      OTHER="$STAGE/install-other/lib/$SDL_BASE"
      [ -f "$OTHER" ] || { echo "--lipo: $OTHER missing (build both with -DX2_FETCH_SDL3=ON)" >&2; exit 1; }
      lipo -create "$APP/Contents/Frameworks/$SDL_BASE" "$OTHER" -output "$STAGE/sdl.universal"
      mv "$STAGE/sdl.universal" "$APP/Contents/Frameworks/$SDL_BASE"
    fi
    install_name_tool -id "@rpath/$SDL_BASE" "$APP/Contents/Frameworks/$SDL_BASE"
    [ "$SDL_REF" = "@rpath/$SDL_BASE" ] || install_name_tool -change "$SDL_REF" "@rpath/$SDL_BASE" "$EXE"
    for r in $(otool -l "$EXE" | awk '/LC_RPATH/ {getline; getline; print $2}' | sort -u); do # once per fat file
      install_name_tool -delete_rpath "$r" "$EXE"
    done
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$EXE"
  fi

  MIN_MACOS=$(otool -l "$EXE" | awk '/LC_BUILD_VERSION/ {f=1} f && $1 == "minos" {print $2; exit}')
  sed -e "s/@VERSION@/$VERSION/g" -e "s/@MIN_MACOS@/${MIN_MACOS:-11.0}/g" \
      -e "s/@BUNDLE_ID@/${X2_BUNDLE_ID:-io.github.xenon2-recomp.x2recomp}/g" \
      "$ROOT/packaging/Info.plist.in" > "$APP/Contents/Info.plist"
  plutil -lint "$APP/Contents/Info.plist" > /dev/null
  printf 'APPL????' > "$APP/Contents/PkgInfo"

  # Generated icon (no game art): PNG -> iconset -> icns.
  PY=$(command -v python3 || command -v python || true)
  if [ -n "$PY" ] && command -v iconutil > /dev/null 2>&1; then
    SET="$STAGE/x2recomp.iconset"
    mkdir -p "$SET"
    "$PY" -I "$ROOT/packaging/make_icon.py" "$STAGE/icon.png" 1024
    for s in 16 32 128 256 512; do
      sips -z $s $s "$STAGE/icon.png" --out "$SET/icon_${s}x${s}.png" > /dev/null
      sips -z $((s * 2)) $((s * 2)) "$STAGE/icon.png" --out "$SET/icon_${s}x${s}@2x.png" > /dev/null
    done
    iconutil -c icns "$SET" -o "$APP/Contents/Resources/x2recomp.icns"
  else
    echo "note: python3 or iconutil missing, the app has no icon" >&2
  fi

  # Documents and licences next to the app (also inside it, for a copied-out .app).
  cp -R "$I/share/doc/xenon2_recomp/." "$PKG/"
  cp -R "$I/share/doc/xenon2_recomp/licenses" "$APP/Contents/Resources/licenses"

  # Sign the library first, then the app (install_name_tool invalidated the linker's ad-hoc signature).
  for f in "$APP"/Contents/Frameworks/*.dylib; do [ -f "$f" ] && codesign --force --sign "$SIGN" "$f"; done
  codesign --force --sign "$SIGN" "$APP"
  codesign --verify --strict "$APP"

  check_contents "$PKG"
  write_manifest "$PKG"
  ARCHIVE="$OUT/$NAME.zip"
  rm -f "$ARCHIVE"
  (cd "$STAGE" && ditto -c -k --keepParent "$NAME" "$ARCHIVE")
  if [ "$DMG" = 1 ]; then
    rm -f "$OUT/$NAME.dmg"
    hdiutil create -quiet -volname "x2recomp $VERSION" -srcfolder "$PKG" -format UDZO "$OUT/$NAME.dmg"
    (cd "$OUT" && sha256 "$NAME.dmg" > "$NAME.dmg.sha256")
  fi
else
  NAME="x2recomp-$VERSION-linux-$ARCH"
  PKG="$STAGE/$NAME"
  cp -R "$STAGE/install" "$PKG"
  # The executable finds lib/libSDL3.so.0 through its RUNPATH ($ORIGIN/../lib, set by the install rules).
  if command -v readelf > /dev/null 2>&1; then
    readelf -d "$PKG/bin/x2recomp" | grep -E 'RUNPATH|RPATH' || echo "note: no RUNPATH in x2recomp" >&2
  fi
  [ -n "$(ls "$PKG"/lib*/libSDL3.so* 2> /dev/null)" ] ||
    echo "note: SDL3 is not bundled (a system SDL3 build): users need SDL3 installed (docs/RELEASING.md)" >&2
  check_contents "$PKG"
  write_manifest "$PKG"
  ARCHIVE="$OUT/$NAME.tar.gz"
  tar -C "$STAGE" -czf "$ARCHIVE" "$NAME"
fi
(cd "$OUT" && sha256 "$(basename "$ARCHIVE")" > "$(basename "$ARCHIVE").sha256")
echo "package: $ARCHIVE"
cat "$ARCHIVE.sha256"
