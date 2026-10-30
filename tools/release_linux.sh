#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Builds a Linux release archive in a Docker container and writes it to dist/. docs/RELEASING.md.
#
#   tools/release_linux.sh [--arch aarch64|x86_64] [--ref GITREF] [--out DIR] [--image IMAGE] [--jobs N] [--keep]
#
#   --arch ARCH    target architecture (default: the host's). x86_64 on an arm64 host (or the reverse) needs
#                  QEMU emulation in Docker and takes much longer
#   --ref GITREF   the commit, tag or branch to build (default HEAD). The source is a `git archive` of that
#                  ref, so an uncommitted change is never in a release
#   --out DIR      where the archive goes (default dist/, gitignored)
#   --image IMAGE  build container (default ubuntu:22.04)
#   --jobs N       compiler jobs (default: the container's nproc)
#   --keep         keep the container afterwards, for looking at the logs
#
# The build container is deliberately old: glibc is not bundled, so the archive's glibc floor is the container's.
# ubuntu:22.04 has glibc 2.35, which runs on Debian 12 "bookworm" / 64-bit Raspberry Pi OS (2.36) and later.
# libstdc++ and libgcc are linked statically into x2recomp, so the only versioned symbols left are glibc's.
# SDL3 is the pinned release built from source (X2_FETCH_SDL3=ON) with every backend's development headers
# installed, so the one libSDL3.so in the archive dlopen's X11, Wayland, KMSDRM, ALSA, PulseAudio or PipeWire
# as the user's machine provides them.
#
# No game data, imported assets or translated code can reach the archive: the source is a git archive (which
# holds no ignored file), the build is interpreter-only, and tools/package.sh checks the staged files.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ARCH=$(uname -m)
REF=HEAD
OUT="$ROOT/dist"
IMAGE=ubuntu:22.04
JOBS=
KEEP=0
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCH="$2"; shift ;;
    --ref) REF="$2"; shift ;;
    --out) mkdir -p "$2"; OUT=$(cd "$2" && pwd); shift ;;
    --image) IMAGE="$2"; shift ;;
    --jobs) JOBS="$2"; shift ;;
    --keep) KEEP=1 ;;
    -h|--help) sed -n '4,27p' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
case "$ARCH" in
  aarch64|arm64) ARCH=aarch64; PLATFORM=linux/arm64 ;;
  x86_64|amd64) ARCH=x86_64; PLATFORM=linux/amd64 ;;
  *) echo "--arch must be aarch64 or x86_64, not $ARCH" >&2; exit 2 ;;
esac
command -v docker > /dev/null || { echo "docker is needed" >&2; exit 1; }
COMMIT=$(git -C "$ROOT" rev-parse --short "$REF")

WORK=$(mktemp -d "${TMPDIR:-/tmp}/x2linux.XXXXXX")
CONTAINER="x2-linux-$ARCH-$$"
cleanup() {
  [ "$KEEP" = 1 ] || docker rm -f "$CONTAINER" > /dev/null 2>&1 || true
  rm -rf "$WORK"
}
trap cleanup EXIT

git -C "$ROOT" archive --format=tar --prefix=src/ "$REF" > "$WORK/src.tar"

cat > "$WORK/inside.sh" << 'INSIDE'
#!/bin/sh
# Runs in the build container: /work holds src.tar on the way in and dist/ plus the logs on the way out.
set -eu
export DEBIAN_FRONTEND=noninteractive
echo "container: $(. /etc/os-release && echo "$PRETTY_NAME") $(uname -m), glibc $(ldd --version | sed -n '1s/.* //p')"
apt-get update -qq
# build-essential and binutils for the toolchain; gcc-12 for C++20; the rest are SDL3's backends, whose headers
# decide which video, audio and input drivers the one bundled libSDL3.so can dlopen at run time.
apt-get install -y -qq --no-install-recommends \
  build-essential gcc-12 g++-12 ninja-build python3 python3-venv git ca-certificates pkg-config file binutils \
  libasound2-dev libpulse-dev libpipewire-0.3-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
  libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev \
  libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev \
  libdbus-1-dev libibus-1.0-dev libudev-dev libusb-1.0-0-dev liburing-dev > /work/apt.log 2>&1
# The project needs CMake 3.29; 22.04 ships 3.22. The pinned wheel is the same version on every host.
python3 -m venv /opt/venv
/opt/venv/bin/pip install -q "cmake==3.31.6"
PATH=/opt/venv/bin:$PATH
export PATH
cmake --version | head -1
gcc-12 --version | head -1
JOBS=${JOBS:-$(nproc)}

mkdir -p /build
tar -C /build -xf /work/src.tar
# -static-libstdc++/-static-libgcc on the executables only: libSDL3.so stays a plain C library against libc, and
# x2recomp then carries no GLIBCXX_ or CXXABI_ requirement, which is what breaks first on an older distribution.
cmake -S /build/src -B /build/b -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc-12 -DCMAKE_CXX_COMPILER=g++-12 \
  -DX2_REQUIRE_SDL3=ON -DX2_FETCH_SDL3=ON \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" > /work/configure.log 2>&1 ||
  { echo "configure failed"; tail -40 /work/configure.log; exit 1; }
echo "== SDL3 backends (from its configure summary; _SHARED means dlopen'ed at run time, not linked)"
sed -n '/SDL3 was configured with the following options/,$p' /work/configure.log |
  grep -E 'SDL_(X11|WAYLAND|KMSDRM|OPENGL|VULKAN|ALSA|PULSEAUDIO|PIPEWIRE|RPI|OFFSCREEN)[A-Z_]* *\(Wanted' || true
cmake --build /build/b -j "$JOBS" > /work/build.log 2>&1 ||
  { echo "build failed"; tail -40 /work/build.log; exit 1; }

echo "== tests without game data (data tests report Skipped)"
SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy \
  ctest --test-dir /build/b -j "$JOBS" --output-on-failure > /work/ctest.log 2>&1 || true
grep -E 'tests passed|Failed|\*\*\*' /work/ctest.log || true

cd /build/src
tools/package.sh --build-dir /build/b --out /work/dist

echo "== symbol version floors"
mkdir /work/x
tar -C /work/x -xzf /work/dist/x2recomp-*-linux-*.tar.gz
P=$(echo /work/x/x2recomp-*-linux-*)
# The highest version of each symbol-version set the file asks for: that is the floor of the system it runs on.
floor() { objdump -T "$1" | sed -n "s/.*$2\([0-9][0-9.]*\)[^0-9.].*/\1/p" | sort -V | tail -1; }
for f in "$P"/bin/x2recomp "$P"/lib/libSDL3.so*; do
  [ -f "$f" ] || continue
  for set in GLIBC_ GLIBCXX_ CXXABI_; do
    v=$(floor "$f" "$set")
    printf '%s %s%s\n' "$(basename "$f")" "$set" "${v:-(none)}"
  done
done
echo "== dynamic section"
readelf -d "$P"/bin/x2recomp | grep -E 'NEEDED|RUNPATH|RPATH'
readelf -d "$P"/lib/libSDL3.so* | grep -E 'NEEDED|SONAME'
echo "== it runs"
"$P"/bin/x2recomp --version
INSIDE

echo "building $ARCH from $REF ($COMMIT) in $IMAGE on $PLATFORM"
docker create --platform "$PLATFORM" --name "$CONTAINER" ${JOBS:+-e JOBS="$JOBS"} \
  "$IMAGE" sh /work/inside.sh > /dev/null
docker cp "$WORK/." "$CONTAINER:/work" > /dev/null
docker start -a "$CONTAINER"

mkdir -p "$OUT"
docker cp "$CONTAINER:/work/dist/." "$OUT/" > /dev/null
echo
for f in "$OUT"/x2recomp-*-linux-$ARCH.tar.gz; do
  [ -f "$f" ] || continue
  ls -l "$f"
  cat "$f.sha256"
done
