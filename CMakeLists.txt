# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# 3.29: the game-data test drivers (tests/*.cmake) end with cmake_language(EXIT 77) so that CTest reports them as
# Skipped without the user's data; CMakePresets.json uses schema version 6 (3.25). Ubuntu 24.04's packaged CMake
# is 3.28: use Kitware's apt repository or `pip install cmake` there (docs/BUILDING.md).
cmake_minimum_required(VERSION 3.29)
# The version string's single source of truth (x2recomp --version, window title, packages; docs/RELEASING.md).
project(xenon2_recomp VERSION 0.1.0 DESCRIPTION "Xenon 2 Megablast static recompilation runtime" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No C++20 modules: without this, CMake >= 3.28 with Ninja runs a module dependency scan that needs clang-scan-deps
# (missing from Ubuntu's clang package).
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(X2_FETCH_SDL3 "Download and build the pinned SDL3 release instead of find_package(SDL3)" OFF)
option(X2_REQUIRE_SDL3 "Fail the configure step if SDL3 is not available (instead of a headless-only build)" OFF)
set(X2_M68KMAKE "" CACHE FILEPATH "Prebuilt host m68kmake to run when cross-compiling (default: build and run it)")

# MSVC links the C runtime statically, so a release needs no Visual C++ redistributable: a dynamic CRT makes
# x2recomp.exe import VCRUNTIME140.dll and MSVCP140.dll, which a clean Windows machine does not have. The
# Universal CRT is a system component on Windows 10 and later either way. This must be set before any target is
# created, including SDL3's: an SDL3.dll built /MD would bring the redistributable back by itself.
# docs/PORTING.md explains why a separate CRT in SDL3.dll is safe here.
option(X2_STATIC_CRT "MSVC: link the C runtime statically, so no Visual C++ redistributable is needed" ON)
if(MSVC AND X2_STATIC_CRT)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set(SDL_FORCE_STATIC_VCRT ON CACHE BOOL "" FORCE) # SDL3's own switch for the same thing
endif()

if(MSVC)
  # Plain C/C++ standard library use, no Windows headers; UTF-8 sources; real __cplusplus; conforming mode.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
  add_compile_options(/utf-8 $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus> $<$<COMPILE_LANGUAGE:CXX>:/permissive->)
endif()

# Every executable runs with the UTF-8 active code page on Windows, so that argv and the UTF-8 paths SDL
# returns survive into fopen and std::filesystem. Nothing to do on other platforms. docs/PORTING.md.
# CMake's Windows-GNU platform module already enables the RC language, so the .rc route needs no setup.
function(x2_win_utf8 target)
  if(MSVC)
    target_link_options(${target} PRIVATE
      /MANIFEST:EMBED "/MANIFESTINPUT:${CMAKE_SOURCE_DIR}/packaging/win-utf8.manifest")
  elseif(MINGW)
    target_sources(${target} PRIVATE ${CMAKE_SOURCE_DIR}/packaging/win-utf8.rc)
  endif()
endfunction()

# Warnings for the project's own code (vendored and generated code are compiled without).
function(x2_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W3>)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)
  endif()
endfunction()

# --- Musashi 68000 core (vendor/musashi). m68kops.c/h are generated from m68k_in.c by m68kmake. ---
set(MUSASHI_DIR ${CMAKE_SOURCE_DIR}/vendor/musashi)
set(MUSASHI_GEN ${CMAKE_BINARY_DIR}/musashi_gen)
if(X2_M68KMAKE)
  set(x2_m68kmake_cmd ${X2_M68KMAKE})
elseif(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
  message(FATAL_ERROR "Cross-compiling: build m68kmake for the host first and pass -DX2_M68KMAKE=/path/to/m68kmake")
else()
  add_executable(m68kmake ${MUSASHI_DIR}/m68kmake.c)
  if(MSVC)
    target_compile_options(m68kmake PRIVATE /w)
  endif()
  set(x2_m68kmake_cmd m68kmake)
endif()
add_custom_command(
  OUTPUT ${MUSASHI_GEN}/m68kops.c ${MUSASHI_GEN}/m68kops.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSASHI_GEN}
  COMMAND ${x2_m68kmake_cmd} ${MUSASHI_GEN} ${MUSASHI_DIR}/m68k_in.c
  DEPENDS ${x2_m68kmake_cmd} ${MUSASHI_DIR}/m68k_in.c
  COMMENT "Generating Musashi opcode tables")

# No SoftFloat: runtime/musashi_no_fpu.c stands in for ${MUSASHI_DIR}/softfloat/softfloat.c. Musashi's FPU is
# only reachable for 68030+ CPU types and this is a 68000, so the stubs are unreachable; see that file's
# header and docs/PROVENANCE.md §7.3. Keep these two lines together if you touch this target.
add_library(musashi STATIC
  ${MUSASHI_DIR}/m68kcpu.c
  ${MUSASHI_DIR}/m68kdasm.c
  ${CMAKE_SOURCE_DIR}/runtime/musashi_no_fpu.c
  ${MUSASHI_GEN}/m68kops.c)
# SYSTEM: Musashi's headers are included by the runtime; their warnings are not ours.
target_include_directories(musashi SYSTEM PUBLIC ${MUSASHI_DIR} ${MUSASHI_GEN})
target_include_directories(musashi PUBLIC ${CMAKE_SOURCE_DIR}/runtime)
target_compile_definitions(musashi PUBLIC MUSASHI_CNF="x2_m68kconf.h")
if(MSVC)
  target_compile_options(musashi PRIVATE /w)
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(musashi PRIVATE -w)
endif()

# --- runtime core (no platform dependencies) ---
add_library(x2runtime STATIC
  runtime/boot.cpp
  runtime/cia.cpp
  runtime/custom_regs.cpp
  runtime/debug_dump.cpp
  runtime/disk_shim.cpp
  runtime/machine.cpp
  runtime/machine_timing.cpp
  runtime/persist.cpp
  runtime/retail_protection.cpp
  runtime/trace/tracer.cpp
  runtime/audio/audio_cues.cpp
  runtime/audio/paula.cpp
  runtime/audio/wav_writer.cpp)
target_include_directories(x2runtime PUBLIC runtime)
target_link_libraries(x2runtime PUBLIC musashi)
x2_warnings(x2runtime)

add_executable(x2recomp runtime/main.cpp)
target_link_libraries(x2recomp PRIVATE x2runtime)
x2_win_utf8(x2recomp)
target_compile_definitions(x2recomp PRIVATE X2_VERSION="${PROJECT_VERSION}")
x2_warnings(x2recomp)
if(MINGW) # no libstdc++, libgcc or winpthread DLLs to ship next to x2recomp.exe (SDL3.dll stays a DLL)
  target_link_options(x2recomp PRIVATE -static)
endif()

# --- video (scanline Denise/Agnus model, screenshots; no platform dependencies) ---
add_library(x2gfx STATIC runtime/gfx/video.cpp runtime/gfx/image_io.cpp)
target_link_libraries(x2gfx PUBLIC x2runtime)
x2_warnings(x2gfx)
target_link_libraries(x2recomp PRIVATE x2gfx)

# --- modernisation layer (optional presentation features, off by default; docs/re-notes/modernisation-design.md) ---
add_library(x2modern STATIC runtime/modern/draw_capture.cpp runtime/modern/interp.cpp runtime/modern/present.cpp
  runtime/modern/assets.cpp)
target_link_libraries(x2modern PUBLIC x2runtime x2gfx x2import)
x2_warnings(x2modern)
target_link_libraries(x2recomp PRIVATE x2modern)

# --- disk image import (ADF/ADZ/ZIP -> game_0400.bin + disc2.bin; runtime/import, inflate by vendor/puff) ---
add_library(x2import STATIC runtime/import/importer.cpp runtime/import/disk_image.cpp runtime/import/ext_adf.cpp runtime/import/ipf.cpp
  runtime/import/dms.cpp runtime/import/flux.cpp runtime/import/scp.cpp runtime/import/kryoflux.cpp
  runtime/import/hfe.cpp runtime/import/lha.cpp runtime/import/whdload.cpp runtime/import/sha1.cpp runtime/import/variants.cpp
  runtime/import/retail.cpp runtime/import/crunch.cpp runtime/import/powerpack.cpp vendor/puff/puff.c)
target_include_directories(x2import PUBLIC runtime PRIVATE vendor/puff)
x2_warnings(x2import)
target_link_libraries(x2recomp PRIVATE x2import)

# --- CDTV CD-audio music layer (optional, off by default; runtime/cdtv, docs/re-notes/cdtv-music-map.md) ---
add_library(x2cdtv STATIC runtime/cdtv/inflate_stream.cpp runtime/cdtv/cdtv_disc.cpp runtime/cdtv/cd_player.cpp
  runtime/cdtv/cdtv_music.cpp)
target_link_libraries(x2cdtv PUBLIC x2runtime)
x2_warnings(x2cdtv)
target_link_libraries(x2recomp PRIVATE x2cdtv)

# --- SDL3 (optional: without it only --headless is available) ---
# Default: an installed SDL3 (Homebrew, distro package, vcpkg, or -DCMAKE_PREFIX_PATH=<SDL3 prefix>).
# -DX2_FETCH_SDL3=ON: download the pinned release below and build it as a shared library (needs network at configure
# time; used for macOS x86_64/universal builds and where no SDL3 package exists).
set(X2_SDL3_VERSION 3.4.18)
if(X2_FETCH_SDL3)
  include(FetchContent)
  set(SDL_SHARED ON CACHE BOOL "" FORCE)
  set(SDL_STATIC OFF CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-${X2_SDL3_VERSION}/SDL3-${X2_SDL3_VERSION}.tar.gz
    URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL SYSTEM)
  FetchContent_MakeAvailable(SDL3)
  set(SDL3_FOUND TRUE)
else()
  find_package(SDL3 CONFIG QUIET)
endif()
if(NOT SDL3_FOUND AND X2_REQUIRE_SDL3)
  message(FATAL_ERROR "X2_REQUIRE_SDL3=ON but SDL3 was not found: set CMAKE_PREFIX_PATH or use -DX2_FETCH_SDL3=ON")
endif()

# SDL3 window and input.
if(SDL3_FOUND)
  add_library(x2host STATIC runtime/gfx/sdl_display.cpp runtime/gfx/sdl_filters.cpp runtime/input/input.cpp
    runtime/import/import_screen.cpp)
  target_link_libraries(x2host PUBLIC x2gfx x2import SDL3::SDL3)
  x2_warnings(x2host)
  target_link_libraries(x2recomp PRIVATE x2host)
  target_compile_definitions(x2recomp PRIVATE X2_HAVE_SDL=1)
else()
  message(STATUS "SDL3 not found: building x2recomp without a window (headless only)")
endif()

# SDL3 audio output (the headless runtime and --audio-out need no SDL).
if(SDL3_FOUND)
  add_library(x2audio_sdl STATIC runtime/audio/sdl_audio.cpp)
  target_include_directories(x2audio_sdl PUBLIC runtime)
  target_link_libraries(x2audio_sdl PUBLIC SDL3::SDL3)
  x2_warnings(x2audio_sdl)
  target_link_libraries(x2recomp PRIVATE x2audio_sdl) # window sound output
endif()

# Windows has no rpath: copy SDL3.dll next to each executable that needs it.
function(x2_copy_runtime_dlls target)
  if(WIN32)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:${target}> $<TARGET_RUNTIME_DLLS:${target}>
      COMMAND_EXPAND_LISTS)
  endif()
endfunction()
if(SDL3_FOUND)
  x2_copy_runtime_dlls(x2recomp)
endif()

# --- tests ---
# Every test runs without game data (those that need it report Skipped, exit code 77) and without a display or
# sound device (SDL's dummy drivers below), so CI runners can run the whole suite.
enable_testing()
set(X2_SDL_TEST_ENV "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
add_executable(x2_smoke_tests tests/smoke_tests.cpp)
target_link_libraries(x2_smoke_tests PRIVATE x2runtime)
x2_win_utf8(x2_smoke_tests)
add_test(NAME smoke COMMAND x2_smoke_tests)
add_executable(x2_timing_tests tests/timing_tests.cpp)
target_link_libraries(x2_timing_tests PRIVATE x2runtime)
x2_win_utf8(x2_timing_tests)
add_test(NAME timing COMMAND x2_timing_tests)
add_executable(x2_video_tests tests/video_tests.cpp)
target_link_libraries(x2_video_tests PRIVATE x2gfx)
x2_win_utf8(x2_video_tests)
add_test(NAME video COMMAND x2_video_tests)
add_executable(x2_modern_tests tests/modern_tests.cpp)
target_link_libraries(x2_modern_tests PRIVATE x2modern)
x2_win_utf8(x2_modern_tests)
add_test(NAME modern COMMAND x2_modern_tests ${CMAKE_BINARY_DIR}/modern_test)
if(TARGET x2host)
  add_executable(x2_input_tests tests/input_tests.cpp)
  target_link_libraries(x2_input_tests PRIVATE x2host)
  x2_win_utf8(x2_input_tests)
  add_test(NAME input COMMAND x2_input_tests)
  set_tests_properties(input PROPERTIES ENVIRONMENT "${X2_SDL_TEST_ENV}")
  x2_copy_runtime_dlls(x2_input_tests)
endif()
add_executable(x2_paula_tests tests/audio/paula_tests.cpp)
target_link_libraries(x2_paula_tests PRIVATE x2runtime)
x2_win_utf8(x2_paula_tests)
add_test(NAME paula COMMAND x2_paula_tests)
add_executable(x2_persist_tests tests/persist_tests.cpp)
target_link_libraries(x2_persist_tests PRIVATE x2runtime)
x2_win_utf8(x2_persist_tests)
add_test(NAME persist COMMAND x2_persist_tests ${CMAKE_BINARY_DIR}/persist_test)
add_executable(x2_retail_protection_tests tests/retail_protection_tests.cpp)
target_link_libraries(x2_retail_protection_tests PRIVATE x2runtime x2import)
x2_win_utf8(x2_retail_protection_tests)
add_test(NAME retail_protection COMMAND x2_retail_protection_tests ${CMAKE_BINARY_DIR}/retail_protection_test)
add_executable(x2_build_tests tests/build_tests.cpp)
target_link_libraries(x2_build_tests PRIVATE x2runtime)
x2_win_utf8(x2_build_tests)
add_test(NAME build_table COMMAND x2_build_tests ${CMAKE_BINARY_DIR}/build_table_test)
if(TARGET x2audio_sdl)
  add_executable(x2_sdl_audio_smoke tests/audio/sdl_audio_smoke.cpp)
  target_link_libraries(x2_sdl_audio_smoke PRIVATE x2audio_sdl)
  x2_win_utf8(x2_sdl_audio_smoke)
  add_test(NAME sdl_audio_smoke COMMAND x2_sdl_audio_smoke)
  set_tests_properties(sdl_audio_smoke PROPERTIES ENVIRONMENT "${X2_SDL_TEST_ENV}")
  x2_copy_runtime_dlls(x2_sdl_audio_smoke)
endif()

# CDTV music layer: synthetic CD images, inflater, player, Paula source gains (no game data, no CD audio).
add_executable(x2_cdtv_tests tests/cdtv/cdtv_tests.cpp)
target_link_libraries(x2_cdtv_tests PRIVATE x2cdtv x2import)
x2_win_utf8(x2_cdtv_tests)
add_test(NAME cdtv COMMAND x2_cdtv_tests ${CMAKE_BINARY_DIR}/cdtv_test)

# Importer: synthetic ADF/ADZ/ZIP inputs (no game data), then the user's own Xenon2.zip / Amiga_EN zip if present
# (that part is reported as Skipped without them).
add_executable(x2_import_tests tests/import/import_tests.cpp)
target_link_libraries(x2_import_tests PRIVATE x2import)
x2_win_utf8(x2_import_tests)
add_test(NAME import COMMAND x2_import_tests ${CMAKE_BINARY_DIR}/import_test)
# Disk image layer: DiskImage, container detection, MFM sectors, extended ADF, DMS (synthetic inputs, no data).
add_executable(x2_retail_import_tests tests/import/retail_tests.cpp)
target_link_libraries(x2_retail_import_tests PRIVATE x2import)
x2_win_utf8(x2_retail_import_tests)
add_test(NAME retail_import COMMAND x2_retail_import_tests ${CMAKE_BINARY_DIR}/retail_import_test)
add_executable(x2_disk_image_tests tests/import/disk_image_tests.cpp)
target_link_libraries(x2_disk_image_tests PRIVATE x2import)
add_test(NAME disk_image COMMAND x2_disk_image_tests)
add_executable(x2_flux_tests tests/import/flux_tests.cpp)
target_link_libraries(x2_flux_tests PRIVATE x2import)
add_test(NAME flux COMMAND x2_flux_tests)
# Local only: decode every .scp/.hfe file and trackNN.S.raw set under the folder (real flux dumps; exit 77 if none).
set(X2_FLUX_TEST_DIR "" CACHE PATH "A folder of real flux dumps (SCP, HFE, KryoFlux stream sets) for the flux_files test")
add_test(NAME flux_files COMMAND x2_flux_tests --files "${X2_FLUX_TEST_DIR}")
set_tests_properties(flux_files PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
# Local only: decode every .dms file under the folder (a decoded DMS has passed every per-track CRC and checksum).
# Skipped (exit code 77) when the folder is not set; CI never sets it.
set(X2_DMS_TEST_DIR "" CACHE PATH "A folder of the user's own .dms files for the dms_files test")
add_test(NAME dms_files COMMAND x2_disk_image_tests --dms-dir "${X2_DMS_TEST_DIR}")
set_tests_properties(dms_files PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
# IPF reader: synthetic IPF files (no data); the second test reads real IPFs and is Skipped without a folder.
add_executable(x2_ipf_tests tests/import/ipf_tests.cpp)
target_link_libraries(x2_ipf_tests PRIVATE x2import)
add_test(NAME ipf COMMAND x2_ipf_tests)
# LHA archives: synthetic archives of every header level and method (no data); the second test reads real LHA files.
add_executable(x2_lha_tests tests/import/lha_tests.cpp)
target_link_libraries(x2_lha_tests PRIVATE x2import)
add_test(NAME lha COMMAND x2_lha_tests)
set(X2_LHA_TEST_DIR "" CACHE PATH "A folder of the user's own .lha/.lzh files for the lha_files test")
add_test(NAME lha_files COMMAND x2_lha_tests --lha-dir "${X2_LHA_TEST_DIR}")
set_tests_properties(lha_files PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
set(X2_IPF_TEST_DIR "" CACHE PATH "A folder of the user's own .ipf files for the ipf_files test")
add_test(NAME ipf_files COMMAND x2_ipf_tests --ipf-dir "${X2_IPF_TEST_DIR}")
set_tests_properties(ipf_files PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
set(X2_DISK_ZIPS "${CMAKE_SOURCE_DIR}/Xenon2.zip;${CMAKE_SOURCE_DIR}/Xenon-2-Megablast_Amiga_EN.zip" CACHE STRING
    "The user's own Xenon 2 disk-image archives for the game_import test")
add_test(NAME game_import
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> "-DZIPS=${X2_DISK_ZIPS}"
    -DREFERENCE=${CMAKE_SOURCE_DIR}/generated/assets -DOUT=${CMAKE_BINARY_DIR}/game_import
    -P ${CMAKE_SOURCE_DIR}/tests/import/game_import.cmake)
set_tests_properties(game_import PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)

# Needs user-supplied game data (x2recomp --import or tools/prep_assets.py into X2_ASSETS_DIR). Exit code 77 =
# skipped, reported as "Skipped" by CTest.
set(X2_ASSETS_DIR ${CMAKE_SOURCE_DIR}/generated/assets CACHE PATH "Directory with game_0400.bin and disc2.bin")
add_test(NAME game_boot
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DOUT=${CMAKE_BINARY_DIR}/game_boot
    -P ${CMAKE_SOURCE_DIR}/tests/game_boot.cmake)
set_tests_properties(game_boot PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
# CDTV music layer on the user's own CDTV disc image (Skipped without it): import, cue sequence over replays, game state
# unchanged, exact audio. The import (about 680 MB) goes to X2_CDTV_TEST_DIR, shared by the build directories.
set(X2_CDTV_ZIP "${CMAKE_SOURCE_DIR}/Xenon-2-Megablast_CDTV_EN-FR-DE-IT_Disc-Image.zip" CACHE FILEPATH
    "The user's own Xenon 2 CDTV disc image for the cdtv_game test")
set(X2_CDTV_TEST_DIR "${CMAKE_SOURCE_DIR}/generated/cdtv_test" CACHE PATH "Where cdtv_game imports the CD audio")
add_test(NAME cdtv_game
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DZIP=${X2_CDTV_ZIP} -DASSETS=${X2_ASSETS_DIR} -DREPLAYS=${CMAKE_SOURCE_DIR}/tests/replays
    -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/cdtv/golden -DOUT=${CMAKE_BINARY_DIR}/cdtv_game -DCDTV_DIR=${X2_CDTV_TEST_DIR}
    -DRECOMP=$<BOOL:${X2_RECOMP}>
    -P ${CMAKE_SOURCE_DIR}/tests/cdtv/cdtv_game.cmake)
set_tests_properties(cdtv_game PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 RUN_SERIAL TRUE)
find_package(Python3 COMPONENTS Interpreter QUIET)
# Repository hygiene: no game data, disk images, audio or generated/translated code tracked by git
# (tools/check_no_game_data.py, with its own unit tests). Needs Python 3; the git check is Skipped outside a checkout.
if(Python3_FOUND)
  add_test(NAME check_no_game_data_selftest COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/check_no_game_data.py --self-test)
  add_test(NAME check_no_game_data COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/check_no_game_data.py --repo ${CMAKE_SOURCE_DIR})
  set_tests_properties(check_no_game_data PROPERTIES SKIP_RETURN_CODE 77)
endif()
add_test(NAME game_audio
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DOUT=${CMAKE_BINARY_DIR}/game_audio
    -DPYTHON=${Python3_EXECUTABLE} -DCHECK=${CMAKE_SOURCE_DIR}/tests/audio/check_wav.py
    -P ${CMAKE_SOURCE_DIR}/tests/audio/game_audio.cmake)
set_tests_properties(game_audio PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
# The tune's own rests and the effect channel handover (docs/re-notes/music-dropouts.md).
add_test(NAME music_gaps
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DOUT=${CMAKE_BINARY_DIR}/music_gaps
    -DREPLAY=${CMAKE_SOURCE_DIR}/tests/replays/level1_play.txt
    -DPYTHON=${Python3_EXECUTABLE} -DCHECK=${CMAKE_SOURCE_DIR}/tests/audio/check_music_gaps.py
    -P ${CMAKE_SOURCE_DIR}/tests/audio/music_gaps.cmake)
set_tests_properties(music_gaps PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
# --data-dir moves settings.cfg, input.cfg and highscores.bin too; headless runs write none of them.
if(SDL3_FOUND)
  add_test(NAME data_dir
    COMMAND ${CMAKE_COMMAND}
      -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DOUT=${CMAKE_BINARY_DIR}/data_dir_test
      -P ${CMAKE_SOURCE_DIR}/tests/data_dir.cmake)
  set_tests_properties(data_dir PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300 ENVIRONMENT "${X2_SDL_TEST_ENV}")
endif()

# Golden frame hashes of the scripted run (tests/golden/game_frames.txt). Skipped without game data.
add_test(NAME golden_frames
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DOUT=${CMAKE_BINARY_DIR}/golden_frames
    -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/golden/game_frames.txt
    -P ${CMAKE_SOURCE_DIR}/tests/golden_frames.cmake)
set_tests_properties(golden_frames PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)

# Modernisation spike on the real game: interpolation endpoints exact; with X2_RECOMP also the draw capture
# identical under the translated executor and --interp-only. Skipped without game data.
add_test(NAME modern_game
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DOUT=${CMAKE_BINARY_DIR}/modern_game
    -DREPLAY=${CMAKE_SOURCE_DIR}/tests/replays/level1_play.txt -DRECOMP=$<BOOL:${X2_RECOMP}>
    -P ${CMAKE_SOURCE_DIR}/tests/modern_game.cmake)
set_tests_properties(modern_game PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)

# Modernisation presenter (Phases 9b/9c): endpoint exactness on every replay, Faithful pictures identical to the
# rendered frames, golden hashes of interpolated pictures. Skipped without game data.
file(GLOB X2_MODERN_REPLAYS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/replays/*.txt)
foreach(replay IN LISTS X2_MODERN_REPLAYS)
  get_filename_component(replay_name ${replay} NAME_WE)
  set(min_pairs 1)
  if(replay_name STREQUAL "attract") # title and credits only: nothing to interpolate
    set(min_pairs 0)
  endif()
  add_test(NAME modern_check_${replay_name}
    COMMAND ${CMAKE_COMMAND}
      -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DREPLAY=${replay} -DMIN_PAIRS=${min_pairs}
      -P ${CMAKE_SOURCE_DIR}/tests/modern_check.cmake)
  set_tests_properties(modern_check_${replay_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endforeach()
add_test(NAME modern_faithful
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR}
    -DREPLAY=${CMAKE_SOURCE_DIR}/tests/replays/level1_play.txt
    -P ${CMAKE_SOURCE_DIR}/tests/modern_faithful.cmake)
set_tests_properties(modern_faithful PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
add_test(NAME modern_midpoints
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DREPLAYS=${CMAKE_SOURCE_DIR}/tests/replays
    -DOUT=${CMAKE_BINARY_DIR}/modern_midpoints -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/golden/modern_midpoints.txt
    -P ${CMAKE_SOURCE_DIR}/tests/modern_midpoints.cmake)
set_tests_properties(modern_midpoints PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
# Upgraded graphics (Phases 9d-9f): the dump as a pack reproduces the original pictures; the surround keeps the
# rendered frame in its centre; golden hashes of surround and HD panel pictures. Skipped without game data.
add_test(NAME modern_upgrade
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DOUT=${CMAKE_BINARY_DIR}/modern_upgrade
    -DREPLAY=${CMAKE_SOURCE_DIR}/tests/replays/level1_play.txt -DRECOMP=$<BOOL:${X2_RECOMP}>
    -P ${CMAKE_SOURCE_DIR}/tests/modern_upgrade.cmake)
set_tests_properties(modern_upgrade PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
add_test(NAME modern_wide
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR}
    -DREPLAY=${CMAKE_SOURCE_DIR}/tests/replays/level1_play.txt
    -P ${CMAKE_SOURCE_DIR}/tests/modern_wide.cmake)
set_tests_properties(modern_wide PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
add_test(NAME modern_wide_golden
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DREPLAYS=${CMAKE_SOURCE_DIR}/tests/replays
    -DOUT=${CMAKE_BINARY_DIR}/modern_wide_golden -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/golden/modern_wide.txt
    -P ${CMAKE_SOURCE_DIR}/tests/modern_midpoints.cmake)
set_tests_properties(modern_wide_golden PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)

# --- static recompiler (Phase 5a, docs/re-notes/recompiler-design.md) ---
# Off by default: the interpreter-only build needs no generated code. With -DX2_RECOMP=ON, generate the translated
# C++ first (python3 -I tools/recomp/recomp.py, from your own game data; output in generated/recomp, gitignored).
option(X2_RECOMP "Link the recompiled game code from generated/recomp (hybrid executor, --interp-only to bypass)" OFF)
if(X2_RECOMP)
  set(X2_RECOMP_DIR ${CMAKE_SOURCE_DIR}/generated/recomp CACHE PATH "Output of tools/recomp/recomp.py")
  if(NOT EXISTS ${X2_RECOMP_DIR}/sources.cmake)
    message(FATAL_ERROR "X2_RECOMP=ON but ${X2_RECOMP_DIR}/sources.cmake is missing: run python3 -I tools/recomp/recomp.py")
  endif()
  include(${X2_RECOMP_DIR}/sources.cmake)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${X2_RECOMP_DIR}/sources.cmake)
  # Generated code is Musashi's handler bodies (C idioms, unused variables): no warnings for it.
  set_source_files_properties(${X2_RECOMP_SOURCES} PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")
  add_library(x2recomp_code STATIC runtime/recomp/executor.cpp ${X2_RECOMP_SOURCES})
  if(MSVC)
    target_compile_options(x2recomp_code PRIVATE /bigobj) # thousands of functions per generated file
  endif()
  target_include_directories(x2recomp_code PRIVATE ${X2_RECOMP_DIR})
  target_link_libraries(x2recomp_code PUBLIC x2runtime)
  target_compile_definitions(x2recomp PRIVATE X2_HAVE_RECOMP=1)
  target_link_libraries(x2recomp PRIVATE x2recomp_code)

  # Per-function differential test against the interpreter (needs the game images the code was generated from).
  add_executable(x2_recomp_difftest tests/recomp/difftest.cpp)
  target_link_libraries(x2_recomp_difftest PRIVATE x2recomp_code)
  x2_win_utf8(x2_recomp_difftest)
  add_test(NAME recomp_difftest COMMAND x2_recomp_difftest --assets ${X2_ASSETS_DIR})
  set_tests_properties(recomp_difftest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
  # Full-game gate: scripted title -> menu -> level 1, interpreter vs hybrid, frames/register logs/audio identical.
  add_test(NAME recomp_equivalence
    COMMAND ${CMAKE_COMMAND}
      -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DOUT=${CMAKE_BINARY_DIR}/recomp_equivalence
      -DMIN_COVERAGE=90
      -P ${CMAKE_SOURCE_DIR}/tests/recomp/equivalence.cmake)
  set_tests_properties(recomp_equivalence PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
  # Same comparison (plus coverage) on every replay of the corpus (tests/replays/*.txt).
  file(GLOB X2_RECOMP_REPLAYS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/replays/*.txt)
  foreach(replay IN LISTS X2_RECOMP_REPLAYS)
    get_filename_component(replay_name ${replay} NAME_WE)
    add_test(NAME recomp_replay_${replay_name}
      COMMAND ${CMAKE_COMMAND}
        -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DREPLAY=${replay}
        -DOUT=${CMAKE_BINARY_DIR}/recomp_replay/${replay_name} -DMIN_COVERAGE=90
        -P ${CMAKE_SOURCE_DIR}/tests/recomp/replay_equivalence.cmake)
    set_tests_properties(recomp_replay_${replay_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
    if(replay_name STREQUAL "boss1") # the god-mode hook moves the PC: translated and interpreted must agree
      add_test(NAME cheats_god_mode_equivalence
        COMMAND ${CMAKE_COMMAND}
          -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DMODE=equivalence -DREPLAY=${replay}
          -P ${CMAKE_SOURCE_DIR}/tests/cheats_check.cmake)
      set_tests_properties(cheats_god_mode_equivalence PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
    endif()
  endforeach()
endif()

# Replay corpus (tests/replays/*.txt, see tests/replays/README.md): frame and audio hashes against
# tests/replays/<name>.hashes. Skipped without game data.
file(GLOB X2_REPLAYS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/replays/*.txt)
foreach(replay IN LISTS X2_REPLAYS)
  get_filename_component(replay_name ${replay} NAME_WE)
  add_test(NAME replay_${replay_name}
    COMMAND ${CMAKE_COMMAND}
      -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DREPLAY=${replay}
      -DHASHES=${CMAKE_SOURCE_DIR}/tests/replays/${replay_name}.hashes
      -P ${CMAKE_SOURCE_DIR}/tests/replay_check.cmake)
  set_tests_properties(replay_${replay_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()

# Retail pressings (docs/re-notes/retail-protection.md): images decoded from the original discs by
# tools/retail_assets.py (or the importer, later), each with its own recorded hash manifests in
# tests/replays/retail/<pressing>/. The canonical frames cannot be compared because the retail credits text
# differs (and in pressing 0297 one routine). Skipped without the assets.
set(X2_RETAIL_2874_ASSETS ${CMAKE_SOURCE_DIR}/generated/assets-retail-2874 CACHE PATH
  "Retail pressing SPS 2874 assets (game_0400.bin, disc2.bin, copylock.txt)")
set(X2_RETAIL_0297_ASSETS ${CMAKE_SOURCE_DIR}/generated/assets-retail-0297 CACHE PATH
  "Retail pressing SPS 0297 assets (game_0400.bin, disc2.bin, copylock.txt)")
foreach(pressing 2874 0297)
  foreach(replay_name attract boss1 ending_natural game_over level1_play level2 level3 level4_boss level5 shop two_player)
    add_test(NAME retail${pressing}_${replay_name}
      COMMAND ${CMAKE_COMMAND}
        -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_RETAIL_${pressing}_ASSETS}
        -DREPLAY=${CMAKE_SOURCE_DIR}/tests/replays/${replay_name}.txt
        -DHASHES=${CMAKE_SOURCE_DIR}/tests/replays/retail/${pressing}/${replay_name}.hashes
        -P ${CMAKE_SOURCE_DIR}/tests/replay_check.cmake)
    set_tests_properties(retail${pressing}_${replay_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
  endforeach()
endforeach()
# The other game builds the runtime can start (docs/re-notes/build-variants.md): the Power Pack single-disk release
# (SPS 2234) and the Bombuzal + Xenon 2 compilation (SPS 1762). Assets come from tools/variants/write_assets.py, each
# with a build= manifest; goldens in tests/replays/<build>/. Skipped without the assets.
set(X2_POWER_PACK_ASSETS ${CMAKE_SOURCE_DIR}/generated/assets-power-pack-2234 CACHE PATH
  "Power Pack (SPS 2234) assets (game_0400.bin, disc2.bin, manifest.txt with build=power-pack-2234)")
set(X2_COMPILATION_1762_ASSETS ${CMAKE_SOURCE_DIR}/generated/assets-compilation-1762 CACHE PATH
  "Bombuzal + Xenon 2 (SPS 1762) assets (game_0400.bin, disc2.bin, manifest.txt with build=compilation-1762)")
foreach(build_dir IN ITEMS power-pack-2234 compilation-1762)
  if(build_dir STREQUAL "power-pack-2234")
    set(build_assets ${X2_POWER_PACK_ASSETS})
  else()
    set(build_assets ${X2_COMPILATION_1762_ASSETS})
  endif()
  foreach(replay_name attract boss1 ending_natural game_over level1_play level2 level3 level4_boss level5 shop two_player)
    add_test(NAME build_${build_dir}_${replay_name}
      COMMAND ${CMAKE_COMMAND}
        -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${build_assets}
        -DREPLAY=${CMAKE_SOURCE_DIR}/tests/replays/${replay_name}.txt
        -DHASHES=${CMAKE_SOURCE_DIR}/tests/replays/${build_dir}/${replay_name}.hashes
        -P ${CMAKE_SOURCE_DIR}/tests/replay_check.cmake)
    set_tests_properties(build_${build_dir}_${replay_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
  endforeach()
endforeach()
add_test(NAME retail_without_receipt
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_RETAIL_2874_ASSETS} -DOUT=${CMAKE_BINARY_DIR}/retail_without_receipt
    -P ${CMAKE_SOURCE_DIR}/tests/retail_check.cmake)
set_tests_properties(retail_without_receipt PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# The native retail import with real retail disc pairs (IPF or flux dumps of your own discs; docs/re-notes/retail-protection.md):
# byte-identical image, receipt, manifest, the recorded retail replay hashes, --diagnose-image and the opt-in protection
# patch (own recorded hashes in tests/replays/retail-patched/). Skipped without the files.
set(X2_RETAIL_2874_DISK1 "${CMAKE_SOURCE_DIR}/generated/sources-extra/ipf/2874/Xenon2-Megablast_Disk1.ipf" CACHE FILEPATH
  "Retail disc 1 of pressing SPS 2874 (IPF/SCP/HFE)")
set(X2_RETAIL_2874_DISK2 "${CMAKE_SOURCE_DIR}/generated/sources-extra/ipf/2874/Xenon2-Megablast_Disk2.ipf" CACHE FILEPATH
  "Retail disc 2 of pressing SPS 2874")
set(X2_RETAIL_0297_DISK1 "${CMAKE_SOURCE_DIR}/generated/sources-extra/ipf/0297/Xenon2_Disk1.ipf" CACHE FILEPATH
  "Retail disc 1 of pressing SPS 0297 (IPF/SCP/HFE)")
set(X2_RETAIL_0297_DISK2 "${CMAKE_SOURCE_DIR}/generated/sources-extra/ipf/0297/Xenon2_Disk2.ipf" CACHE FILEPATH
  "Retail disc 2 of pressing SPS 0297")
# pressing | decoded game image sha1 | sha1 of the image with the protection patch applied
set(X2_RETAIL_IMPORT_CASES
  "2874|fc84cc6f1ed47e3da0d5dc913632c705140f1e0a|4ea8c57dc928bc906e3cb8a0d6e1120d707b6320"
  "0297|8d02d25be543d21d3545b321ec774b41afc9d7e3|1ca85d734896572f954c156085846032c3efb8b7")
foreach(case IN LISTS X2_RETAIL_IMPORT_CASES)
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 pressing)
  list(GET parts 1 game_sha1)
  list(GET parts 2 patched_sha1)
  add_test(NAME retail_import_${pressing}
    COMMAND ${CMAKE_COMMAND}
      -DX2RECOMP=$<TARGET_FILE:x2recomp> -DDISK1=${X2_RETAIL_${pressing}_DISK1} -DDISK2=${X2_RETAIL_${pressing}_DISK2}
      -DPRESSING=retail-${pressing} -DPRESSING_NUMBER=${pressing} -DGAME_SHA1=${game_sha1} -DPATCHED_SHA1=${patched_sha1}
      -DREPLAYS=${CMAKE_SOURCE_DIR}/tests/replays -DOUT=${CMAKE_BINARY_DIR}/retail_import_${pressing}
      -P ${CMAKE_SOURCE_DIR}/tests/retail_import.cmake)
  set_tests_properties(retail_import_${pressing} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endforeach()

# The variant registry with real disk images (the BS1 family, Timecircle, "Xenon 2 & 4 Others", the cracked retail disks,
# the refusals, a WHDLoad install of the first retail pressing): see tests/variants_import.cmake. Skipped without the images.
set(X2_EXTRA_SOURCES_DIR "${CMAKE_SOURCE_DIR}/generated/sources-extra" CACHE PATH
  "Folder holding tosec2010-adf/ and whd/ with the variant disk images (docs/re-notes/sources.md)")
add_test(NAME variants_import
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DSRC=${X2_EXTRA_SOURCES_DIR} -DDISC2=${X2_ASSETS_DIR}/disc2.bin
    -DREPLAYS=${CMAKE_SOURCE_DIR}/tests/replays -DOUT=${CMAKE_BINARY_DIR}/variants_import
    -P ${CMAKE_SOURCE_DIR}/tests/variants_import.cmake)
set_tests_properties(variants_import PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)

# The same two replays with --protect-music (docs/re-notes/music-dropouts.md §8). Their frame hashes are the
# faithful ones, which is the point: the enhancement is audio-only. Only the audio hashes differ.
foreach(replay_name level1_play boss1)
  add_test(NAME protect_music_${replay_name}
    COMMAND ${CMAKE_COMMAND}
      -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR}
      -DREPLAY=${CMAKE_SOURCE_DIR}/tests/replays/${replay_name}.txt
      -DHASHES=${CMAKE_SOURCE_DIR}/tests/audio/${replay_name}.protect.hashes -DEXTRA=--protect-music
      -P ${CMAKE_SOURCE_DIR}/tests/replay_check.cmake)
  set_tests_properties(protect_music_${replay_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()

# God mode against the replays that kill the ship (tests/cheats_check.cmake). Skipped without game data.
# game_over reaches GAME OVER by running the energy out; boss1 reaches the one death that costs no energy,
# the push-back sequence at $5FE0. The equivalence mode needs the translated executor, so it is registered
# with the other recomp tests above.
add_test(NAME cheats_god_mode
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DMODE=energy
    -DREPLAY=${CMAKE_SOURCE_DIR}/tests/replays/game_over.txt
    -P ${CMAKE_SOURCE_DIR}/tests/cheats_check.cmake)
set_tests_properties(cheats_god_mode PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
add_test(NAME cheats_god_mode_crush
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DMODE=crush
    -DREPLAY=${CMAKE_SOURCE_DIR}/tests/replays/boss1.txt
    -P ${CMAKE_SOURCE_DIR}/tests/cheats_check.cmake)
set_tests_properties(cheats_god_mode_crush PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)

# --- install and packages (tools/package.sh / tools/package.ps1 build the release archives; docs/RELEASING.md) ---
# Installs the executable, SDL3 (when it is a shared library), the documentation, LICENSE (GPL-3.0-or-later) and
# the third-party licence texts. Never game
# data, imported assets or generated/translated code.
include(GNUInstallDirs)
set(X2_DOC_DIR ${CMAKE_INSTALL_DOCDIR})
if(WIN32)
  set(X2_DOC_DIR .)
endif()
if(APPLE)
  set_target_properties(x2recomp PROPERTIES INSTALL_RPATH "@executable_path/../${CMAKE_INSTALL_LIBDIR}")
elseif(UNIX)
  set_target_properties(x2recomp PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
install(TARGETS x2recomp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(SDL3_FOUND)
  if(X2_FETCH_SDL3)
    install(TARGETS SDL3-shared RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      NAMELINK_SKIP)
  elseif(TARGET SDL3::SDL3-shared)
    install(IMPORTED_RUNTIME_ARTIFACTS SDL3::SDL3-shared RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  endif()
endif()
install(FILES LICENSE README.md THIRD-PARTY-NOTICES.md docs/PROVENANCE.md docs/BUILDING.md docs/ASSET-PACKS.md
  docs/PORTING.md DESTINATION ${X2_DOC_DIR})
install(FILES packaging/licenses/README.txt packaging/licenses/Musashi-LICENSE.txt
  packaging/licenses/puff-LICENSE.txt DESTINATION ${X2_DOC_DIR}/licenses)
if(SDL3_FOUND)
  install(FILES packaging/licenses/SDL3-LICENSE.txt DESTINATION ${X2_DOC_DIR}/licenses)
endif()

set(CPACK_PACKAGE_NAME x2recomp)
set(CPACK_PACKAGE_VENDOR "xenon2-recomp")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY ${PROJECT_DESCRIPTION})
set(CPACK_RESOURCE_FILE_README ${CMAKE_SOURCE_DIR}/README.md)
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
set(CPACK_STRIP_FILES ON)
if(WIN32)
  set(CPACK_GENERATOR ZIP)
else()
  set(CPACK_GENERATOR TGZ)
endif()
include(CPack)
