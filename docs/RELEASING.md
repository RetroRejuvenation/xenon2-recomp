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

1. Choose the project's licence (a maintainer decision: see `README.md`). Until then, packages carry
   `packaging/LICENSE-TODO.txt`, which says that no licence is granted for the project's own code.
2. Bump `project(... VERSION ...)` in `CMakeLists.txt`; describe the changes in the commit message.
3. Run the full test suite with your own game data on at least one platform (`docs/PORTING.md`, "Running the
   data-dependent tests"), both builds:

   ```
   cmake -S . -B build && cmake --build build && ctest --test-dir build
   cmake -S . -B build-recomp -DX2_RECOMP=ON && cmake --build build-recomp && ctest --test-dir build-recomp
   ```

4. Check that CI is green on every platform (`.github/workflows/ci.yml`; game-data tests are Skipped there).
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
- `README.md`, `BUILDING.md`, `ASSET-PACKS.md`, `PORTING.md` and `licenses/`;
- `SHA256SUMS.txt` of all of the above.

Other ways to build it:

- A single-architecture package: build with the `macos-arm64` preset (Homebrew SDL3, which is arm64 only) or
  `macos-x86_64`, then `tools/package.sh --build-dir build-macos-arm64`.
- Universal from two single-architecture builds: `tools/package.sh --build-dir A --lipo B`. Both builds must use
  the pinned SDL3 (`-DX2_FETCH_SDL3=ON`), because Homebrew's SDL3 is arm64 only.

By default the app is signed ad hoc (`--sign -`). That is enough to run it on the machine that built it.
Gatekeeper on other Macs rejects an ad hoc signed download unless the user removes the quarantine attribute.
For a public release, sign with a Developer ID (`--sign`) and notarise the zip or dmg (`xcrun notarytool submit`,
then `xcrun stapler staple`). This needs the maintainer's Apple developer account and is not automated here. The
bundle identifier defaults to `io.github.xenon2-recomp.x2recomp`; override it with
`X2_BUNDLE_ID=... tools/package.sh`.

### Linux

```
cmake --preset linux-gcc -DX2_FETCH_SDL3=ON && cmake --build --preset linux-gcc
tools/package.sh --build-dir build-linux-gcc
```

This writes `dist/x2recomp-<version>-linux-<arch>.tar.gz`, which holds:

- `bin/x2recomp`, with RUNPATH `$ORIGIN/../lib`;
- `lib/libSDL3.so.0`;
- `share/doc/xenon2_recomp/`, with the documents and `licenses/`;
- `SHA256SUMS.txt`.

With a distribution's SDL3 instead of the pinned one (`find_package`), the install rules still copy that
`libSDL3.so.0` into the package. To rely on the system SDL3 instead, remove `lib/` from the package and say so in
the release notes.

Build on the oldest distribution you want to support: glibc is not bundled. Ubuntu 24.04 needs CMake 3.29 or
newer from pip or Kitware's apt repository.

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

The executable is a console program. Started from Explorer, it opens a console window next to the game window
(`docs/PORTING.md`). Code signing (Authenticode) is not set up. Neither this script nor the Windows build has
been run by the project yet: check the first Windows package by hand.

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
