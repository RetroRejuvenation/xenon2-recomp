# Releasing x2recomp

A release is the interpreter build of `x2recomp` with SDL3, its documents and the licence texts, for macOS,
Linux and Windows. A release never contains game data, imported assets, disk or CD images, ROMs, picture or audio
dumps, or code generated from the game (`generated/`, `-DX2_RECOMP=ON` builds). `tools/package.sh` and
`tools/package.ps1` refuse recompiled build directories and check the staged files before archiving.

## Version

The single source of truth is `project(xenon2_recomp VERSION x.y.z ...)` in `CMakeLists.txt`. CMake passes it
to the executable as `X2_VERSION`, which is shown by:

- `x2recomp --version` and the first line of `x2recomp --help`;
- the window title (`Xenon 2 Megablast - x2recomp x.y.z`) until the first picture-option change retitles it;
- the package names (`x2recomp-x.y.z-<os>-<arch>`), `CPACK_PACKAGE_VERSION` and the macOS `Info.plist`.

The importer's own format version (`kImporterVersion` in `runtime/import/importer.cpp`) and the CDTV manifest
version are separate: bump them only when imported data must be re-imported.

The current version, 0.1.0, is a placeholder until the first release; choosing the numbering scheme is the
maintainer's decision.

## Checklist

1. Check `LICENSE` is in the package and that `THIRD-PARTY-NOTICES.md` and `PROVENANCE.md` are next to
   `README.md`. The project is GPL-3.0-or-later; `docs/PROVENANCE.md` §7 has the reasoning.
2. Bump `project(... VERSION ...)` in `CMakeLists.txt`; describe the changes in the commit message.
3. Run the full test suite with your own game data on at least one platform (`docs/PORTING.md`, "Running the
   data-dependent tests"), both builds:

   ```
   cmake -S . -B build && cmake --build build && ctest --test-dir build
   cmake -S . -B build-recomp -DX2_RECOMP=ON && cmake --build build-recomp && ctest --test-dir build-recomp
   ```

4. Check that CI is green on every platform (`.github/workflows/ci.yml`; game-data tests are Skipped there).
   Two jobs, `macOS x86_64` and `Windows MinGW-w64`, are `continue-on-error`: they do not fail the workflow,
   so read their individual results rather than the overall tick. Note also that a documentation-only push
   does not trigger CI at all (`paths-ignore`), so make sure the commit you are releasing from actually had a
   run. As of run `41041824614` (commit `95d9d78`) all eight jobs pass.
5. Tag the commit: `git tag -a vX.Y.Z -m "x2recomp X.Y.Z"`.
6. Build the packages from clean build directories (below), one per platform, and note the `.sha256` lines.
7. Publish the archives with their `.sha256` files and the `SHA256SUMS.txt` listing.

## Packages

### macOS

```
cmake --preset macos-universal && cmake --build --preset macos-universal     # arm64 + x86_64, pinned SDL3
tools/package.sh --build-dir build-macos-universal [--dmg] [--sign "Developer ID Application: ..."]
```

This writes `dist/x2recomp-<version>-macos-universal.zip` (and `.dmg` with `--dmg`). The zip holds:

- `x2recomp.app`, with `Contents/MacOS/x2recomp`, `Contents/Frameworks/libSDL3.0.dylib` (install name
  `@rpath/libSDL3.0.dylib`, executable rpath `@executable_path/../Frameworks`), `Info.plist`, a generated icon
  (`packaging/make_icon.py`: a geometric arrow, no game art) and the licence texts in `Contents/Resources`;
- `README.md`, `THIRD-PARTY-NOTICES.md`, `PROVENANCE.md`, `BUILDING.md`, `ASSET-PACKS.md`, `PORTING.md` and
  `licenses/`;
- `SHA256SUMS.txt` of all of the above.

Other ways to build it:

- A single-architecture package: build with the `macos-arm64` preset (Homebrew SDL3, which is arm64 only) or
  `macos-x86_64`, then `tools/package.sh --build-dir build-macos-arm64`.
- Universal from two single-architecture builds: `tools/package.sh --build-dir A --lipo B`. Both builds must use
  the pinned SDL3 (`-DX2_FETCH_SDL3=ON`), because Homebrew's SDL3 is arm64 only.

**Decision: this project ships unsigned.** The maintainer has no Apple developer account and no Windows
code-signing certificate, and does not plan to get either. The app is signed ad hoc (`--sign -`), which is the
default and is enough to run it on the machine that built it, and on Apple Silicon an ad hoc signature is required
for any binary to run at all. Gatekeeper on other Macs rejects an ad hoc signed download unless the user removes
the quarantine attribute, so the README and release notes tell users how (right-click Open, or
`xattr -dr com.apple.quarantine`). Do not describe the app as notarised or the Windows executable as signed
anywhere. The `--sign "Developer ID Application: ..."` option stays in `tools/package.sh` for anyone who forks the
project and has an account: sign, then notarise the zip or dmg (`xcrun notarytool submit`, then
`xcrun stapler staple`). The bundle identifier defaults to `io.github.xenon2-recomp.x2recomp`; override it with
`X2_BUNDLE_ID=... tools/package.sh`.

### Linux

One command per architecture, from any machine with Docker:

```
tools/release_linux.sh --ref v0.1.0                     # aarch64 (the host's architecture here)
tools/release_linux.sh --ref v0.1.0 --arch x86_64       # QEMU emulation on an arm64 host: slow
```

`tools/release_linux.sh` builds a `git archive` of the ref inside an `ubuntu:22.04` container, runs the test
suite, calls `tools/package.sh`, and writes `dist/x2recomp-<version>-linux-<arch>.tar.gz` with its `.sha256`.
Because the source is a `git archive`, no ignored file — no game data, no imported assets, no `generated/` —
can reach the build, let alone the archive. It prints the symbol-version floors and the SDL3 configure summary
on the way, which is what to read when checking a release. `--keep` leaves the container so you can read
`/work/{configure,build,ctest}.log`.

The archive holds:

- `bin/x2recomp`, with RUNPATH `$ORIGIN/../lib`;
- `lib/libSDL3.so.0.4.18` and the `libSDL3.so.0` symlink the executable asks for;
- `share/doc/xenon2_recomp/`, with the documents and `licenses/`;
- `SHA256SUMS.txt` (regular files only — the symlink is not hashed).

**Why the container is deliberately old.** glibc is not bundled, so the archive's glibc floor is the build
machine's. Ubuntu 22.04 has glibc 2.35, which is below Debian 12 "bookworm" and 64-bit Raspberry Pi OS (2.36);
building on Ubuntu 24.04 (2.39) produces a binary that refuses to start on a Pi with `GLIBC_2.38 not found`.
The same reasoning covers the C++ runtime: the script passes `-static-libstdc++ -static-libgcc` for the
executables, so `x2recomp` carries no `GLIBCXX_` or `CXXABI_` requirement at all and `libSDL3.so` stays a plain
C library. Measured for 0.1.0 on aarch64:

```
x2recomp            GLIBC_2.35   (no GLIBCXX_, no CXXABI_)
libSDL3.so.0.4.18   GLIBC_2.34
x2recomp NEEDED     libSDL3.so.0, libm.so.6, libc.so.6, ld-linux-aarch64.so.1
```

Re-measure after any toolchain change:

```
objdump -T bin/x2recomp | sed -n 's/.*GLIBC_\([0-9.]*\)[^0-9.].*/\1/p' | sort -V | tail -1
```

The floor was checked by running the archive on a system below it: on `ubuntu:20.04` (glibc 2.31) it prints
`version 'GLIBC_2.35' not found (required by .../x2recomp)` and exits 1, which is exactly the failure the
container choice avoids. 2.35 is as low as this tree reaches: Ubuntu 20.04 and Debian 11 (glibc 2.31) are out
of range because their default compilers (GCC 9 and GCC 10) do not have the C++20 this tree uses —
`std::numbers` arrived in GCC 11 — so supporting them would mean a backported toolchain rather than a different
base image. Debian 12 and Ubuntu 22.04 and newer are covered.

**Why all the development packages.** The pinned SDL3 (`X2_FETCH_SDL3=ON`) decides at *configure* time which
backends it can support, and opens them with `dlopen` at run time. The container therefore installs the X11,
Wayland (plus `libdecor`), KMSDRM, OpenGL/GLES/EGL, ALSA, PulseAudio, PipeWire, udev, D-Bus and libusb headers,
so that one `libSDL3.so` picks the right driver on whatever the user has. For 0.1.0 the summary showed X11
(with Xrandr, Xcursor, Xfixes, Xinput, Xscrnsaver, XTest, Xdbe, Xshape, Xsync), Wayland with libdecor, KMSDRM,
OpenGL, OpenGLES, Vulkan and offscreen video; ALSA, PulseAudio, PipeWire, disk and dummy audio — every one of
them `_SHARED`, i.e. `dlopen`ed. `SDL_RPI` (the legacy Broadcom dispmanx backend) is off and should stay off:
it needs the old `/opt/vc` stack that current Raspberry Pi OS does not ship. JACK and sndio are off because
their headers are not installed; add `libjack-jackd2-dev` to the list if a user needs JACK.

GCC 12 is used rather than Ubuntu 22.04's default GCC 11, and it is the oldest compiler the tree has been
built with. It found one real portability bug that GCC 13 had hidden: `runtime/gfx/video.h` used `size_t`
without including `<cstddef>`.

**CI does not check any of this.** The Linux jobs in `.github/workflows/ci.yml` run on `ubuntu-24.04`, which is
the right choice for them — they are there to compile the tree and run the tests — but it means a green run
says nothing about whether a release archive would start on an older system. Only `tools/release_linux.sh`
establishes that, and only by being run.

To build by hand on a Linux machine instead (the archive's floor is then that machine's):

```
cmake --preset linux-gcc -DX2_FETCH_SDL3=ON && cmake --build --preset linux-gcc
tools/package.sh --build-dir build-linux-gcc
```

With a distribution's SDL3 instead of the pinned one (`find_package`), the install rules still copy that
`libSDL3.so.0` into the package. To rely on the system SDL3 instead, remove `lib/` from the package and say so in
the release notes. Ubuntu 24.04 needs CMake 3.29 or newer from pip or Kitware's apt repository.

**Raspberry Pi.** The aarch64 archive is the Pi archive, and `docs/PORTING.md` has a numbered test recipe to
hand to someone with one. 32-bit Raspberry Pi OS (armhf) is not covered by either archive and has never been
built. No Pi has run a release yet.

An AppImage is not produced. If `appimagetool` (and optionally `linuxdeploy`) is available, the same layout
works as an AppDir:

1. Copy the package to `x2recomp.AppDir/usr/`.
2. Add `x2recomp.desktop` (`Exec=x2recomp`, `Icon=x2recomp`, `Categories=Game;`) and a 256x256 icon from
   `python3 packaging/make_icon.py x2recomp.png 256`.
3. Add an `AppRun` symlink to `usr/bin/x2recomp`.
4. Run `appimagetool x2recomp.AppDir`.

This recipe has not been tested.

### Windows

```
cmake --preset windows-msvc && cmake --build --preset windows-msvc
powershell -ExecutionPolicy Bypass -File tools\package.ps1 -BuildDir build-windows-msvc
```

This writes `dist\x2recomp-<version>-windows-x64.zip`, which holds:

- `x2recomp.exe` and `SDL3.dll`;
- the documents and `licenses\`;
- `SHA256SUMS.txt`.

The zip needs nothing installed on the target machine. `X2_STATIC_CRT` (on by default for MSVC) links the C
runtime statically into both `x2recomp.exe` and `SDL3.dll`, so there is no Visual C++ redistributable to
install; the Universal CRT is part of Windows 10 and later. Check this on any new toolchain, because a
dynamically linked CRT fails only on a machine that does not already have Visual Studio:

```
dumpbin /dependents x2recomp.exe   &  dumpbin /dependents SDL3.dll
```

Neither should name `VCRUNTIME140.dll`, `MSVCP140.dll`, `ucrtbase.dll` or any `api-ms-win-crt-*.dll`.

The executable is a console program. Started from Explorer, it opens a console window next to the game window
(`docs/PORTING.md`). The executable is not Authenticode-signed, by decision (see the macOS section), so
SmartScreen warns on first start; the README says how to get past it.
`docs/WINDOWS-VERIFICATION.md` records the first package built and checked on a real Windows machine.

### CPack

The install rules also feed CPack, for a plain archive without the macOS bundle, the icon or the content checks:
`cpack --config build/CPackConfig.cmake` (TGZ on macOS and Linux, ZIP on Windows). `tools/package.sh` and
`tools/package.ps1` are the supported way to build releases.

## Verifying a package

- `shasum -a 256 -c x2recomp-...zip.sha256` (macOS), `sha256sum -c` (Linux), or `Get-FileHash` (Windows).
- Inside the package: `shasum -a 256 -c SHA256SUMS.txt`.
- `x2recomp --version` prints the version. `x2recomp --help` exits with 0.
- macOS: `codesign --verify --strict x2recomp.app`, and
  `otool -L x2recomp.app/Contents/MacOS/x2recomp` must show `@rpath/libSDL3.0.dylib` and no `/opt/homebrew`
  path.
