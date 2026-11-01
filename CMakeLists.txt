# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# 3.29: the game-data test drivers (tests/*.cmake) end with cmake_language(EXIT 77) so that CTest reports them as
# Skipped without the user's data; CMakePresets.json uses schema version 6 (3.25). Ubuntu 24.04's packaged CMake
# is 3.28: use Kitware's apt repository or `pip install cmake` there (docs/BUILDING.md).
cmake_minimum_required(VERSION 3.29)
# The version string's single source of truth (x2recomp --version, window title, packages; docs/RELEASING.md).
project(xenon2_recomp VERSION 0.3.0 DESCRIPTION "Xenon 2 Megablast static recompilation runtime" LANGUAGES C CXX)

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
# --- asset packs v2: the contract between the pack loader and the presenter and audio stages (runtime/pack) ---
add_library(x2pack STATIC runtime/pack/pack_api.cpp runtime/pack/pack_json.cpp runtime/pack/pack_paths.cpp runtime/pack/pack_png.cpp
  runtime/pack/pack_source.cpp runtime/pack/pack_manifest.cpp runtime/pack/pack_stack.cpp runtime/pack/pack_discovery.cpp
  runtime/pack/pack_load.cpp runtime/pack/pack_check.cpp)
target_include_directories(x2pack PUBLIC runtime)
target_link_libraries(x2pack PUBLIC x2import) # crc32 and inflate for PNG and ZIP
target_compile_definitions(x2pack PRIVATE X2_VERSION="${PROJECT_VERSION}") # requires.x2recomp
x2_warnings(x2pack)
target_link_libraries(x2recomp PRIVATE x2pack)

add_library(x2modern STATIC runtime/modern/draw_capture.cpp runtime/modern/layout.cpp runtime/modern/interp.cpp runtime/modern/present.cpp
  runtime/modern/assets.cpp runtime/modern/indexed.cpp runtime/modern/screens.cpp runtime/modern/screen_pack.cpp runtime/modern/screen_dump.cpp)
target_link_libraries(x2modern PUBLIC x2runtime x2gfx x2import x2pack)
x2_warnings(x2modern)
target_link_libraries(x2recomp PRIVATE x2modern)

# --- disk image import (ADF/ADZ/ZIP -> game_0400.bin + disc2.bin; runtime/import, inflate by vendor/puff) ---
add_library(x2import STATIC runtime/import/importer.cpp runtime/import/disk_image.cpp runtime/import/ext_adf.cpp runtime/import/ipf.cpp
  runtime/import/dms.cpp runtime/import/rnc.cpp runtime/import/pdos.cpp runtime/import/flux.cpp runtime/import/fluxengine.cpp runtime/import/scp.cpp runtime/import/kryoflux.cpp
  runtime/import/hfe.cpp runtime/import/lha.cpp runtime/import/whdload.cpp runtime/import/sha1.cpp runtime/import/variants.cpp
  runtime/import/retail.cpp runtime/import/crunch.cpp runtime/import/powerpack.cpp runtime/import/hunk.cpp runtime/import/iso9660.cpp
  runtime/import/cdtv_game.cpp runtime/import/st_disk.cpp runtime/import/st_fat12.cpp runtime/import/st_game.cpp vendor/puff/puff.c)
target_include_directories(x2import PUBLIC runtime PRIVATE vendor/puff)
x2_warnings(x2import)
target_link_libraries(x2recomp PRIVATE x2import)

# --- CDTV CD-audio music layer (optional, off by default; runtime/cdtv, docs/re-notes/cdtv-music-map.md) ---
add_library(x2cdtv STATIC runtime/cdtv/inflate_stream.cpp runtime/cdtv/cdtv_disc.cpp runtime/cdtv/cd_player.cpp
  runtime/cdtv/cdtv_music.cpp runtime/cdtv/cdtv_game.cpp)
target_link_libraries(x2cdtv PUBLIC x2runtime)
x2_warnings(x2cdtv)
target_link_libraries(x2import PUBLIC x2cdtv) # the CDTV game importer reads the disc's data track through cdtv_disc
target_link_libraries(x2recomp PRIVATE x2cdtv)

# --- audio decoding for asset packs: our WAV parser plus the vendored dr_flac (FLAC) and stb_vorbis (Ogg Vorbis) ---
# runtime/audio/decode.*, vendor/audio/README.md, docs/re-notes/audio-decoders.md. Nothing in the runtime uses it yet but the tests.
add_library(x2audiodec STATIC runtime/audio/decode.cpp vendor/audio/dr_flac_impl.c vendor/audio/stb_vorbis_impl.c)
target_include_directories(x2audiodec PUBLIC runtime PRIVATE vendor/audio)
x2_warnings(x2audiodec)
# The vendored decoders are compiled without warnings (as vendor/puff and Musashi are); decode.cpp must not fuse multiply-adds,
# so that the resampler's output is the same on every platform and in every build type.
if(MSVC)
  set_source_files_properties(vendor/audio/dr_flac_impl.c vendor/audio/stb_vorbis_impl.c PROPERTIES COMPILE_OPTIONS /w)
  set_source_files_properties(runtime/audio/decode.cpp PROPERTIES COMPILE_OPTIONS /fp:strict)
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  # -fno-sanitize=shift,float-cast-overflow: the libraries shift negative or sign-bit values left (dr_flac.h le2host_32,
  # stb_vorbis.c) and dr_flac's seek search casts a NaN estimate to an integer that is then clamped; both are harmless in
  # practice, and reporting them would drown the sanitizer fuzz in the vendored code's known noise.
  set_source_files_properties(vendor/audio/dr_flac_impl.c vendor/audio/stb_vorbis_impl.c PROPERTIES COMPILE_OPTIONS "-w;-fno-sanitize=shift,float-cast-overflow")
  set_source_files_properties(runtime/audio/decode.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()

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

# --- Atari ST machine (StMachine: bus, WD1772+DMA, MFP, IKBD, Shifter, YM skeleton; docs/re-notes/atari-st-machine.md) ---
add_library(x2st STATIC runtime/st/mfp.cpp runtime/st/st_disk.cpp runtime/st/fdc.cpp runtime/st/ikbd.cpp runtime/st/shifter.cpp
  runtime/st/ym2149.cpp runtime/st/st_input.cpp runtime/st/st_machine.cpp runtime/st/st_game.cpp runtime/st/st_run.cpp)
target_link_libraries(x2st PUBLIC x2runtime x2gfx x2import x2modern)
x2_warnings(x2st)
if(SDL3_FOUND)
  target_link_libraries(x2st PUBLIC x2host)
  target_compile_definitions(x2st PRIVATE X2_HAVE_SDL=1)
endif()
target_link_libraries(x2recomp PRIVATE x2st)

# The ST machine on synthetic inputs (MFP, WD1772 and DMA, IKBD, Shifter, boot sectors); no game data.
add_executable(x2_st_machine_tests tests/st/st_machine_tests.cpp)
target_link_libraries(x2_st_machine_tests PRIVATE x2st)
x2_win_utf8(x2_st_machine_tests)
x2_warnings(x2_st_machine_tests)
add_test(NAME st_machine COMMAND x2_st_machine_tests)
# The ST's sound and input on synthetic inputs (YM2149 generator, IKBD pause/commands, input scripts, key codes); no game data.
add_executable(x2_st_sound_tests tests/st/st_sound_tests.cpp)
target_link_libraries(x2_st_sound_tests PRIVATE x2st)
x2_win_utf8(x2_st_sound_tests)
x2_warnings(x2_st_sound_tests)
add_test(NAME st_sound COMMAND x2_st_sound_tests)
# The same machine on the user's own Kixx dump (sha1 in the test); Skipped (exit 77) without it.
add_executable(x2_st_game_tests tests/st/st_game_tests.cpp)
target_link_libraries(x2_st_game_tests PRIVATE x2st)
x2_win_utf8(x2_st_game_tests)
x2_warnings(x2_st_game_tests)
add_test(NAME st_game COMMAND x2_st_game_tests)

add_executable(x2_st_kixx_tests tests/st/st_kixx_tests.cpp)
target_link_libraries(x2_st_kixx_tests PRIVATE x2st x2import)
x2_win_utf8(x2_st_kixx_tests)
x2_warnings(x2_st_kixx_tests)
add_test(NAME st_kixx COMMAND x2_st_kixx_tests ${CMAKE_SOURCE_DIR}/generated/st-dev ${CMAKE_BINARY_DIR}/st_kixx_assets)
set_tests_properties(st_kixx PROPERTIES SKIP_RETURN_CODE 77)
foreach(mode god credits levels ending scores)
  add_test(NAME st_hooks_${mode} COMMAND ${CMAKE_COMMAND} -DX2RECOMP=$<TARGET_FILE:x2recomp> -DDISK_DIR=${CMAKE_SOURCE_DIR}/generated/st-dev
           -DREPLAYS=${CMAKE_SOURCE_DIR}/tests/replays/st -DSCRATCH=${CMAKE_BINARY_DIR}/st_hooks_scratch -DMODE=${mode}
           -P ${CMAKE_SOURCE_DIR}/tests/st_hooks_check.cmake)
  set_tests_properties(st_hooks_${mode} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

# The ST replay corpus (tests/replays/st, tools/trace/make_st_replays.py): each replay's frame and audio hashes against the recorded
# manifest on the user's own Kixx dump; Skipped (exit 77) without it. EXPECT pins the end state the replay is meant to reach.
set(X2_ST_REPLAYS attract level1_play level1_flood level1_ff shop shop5 level2_ff level3_ff level4_ff level5_ff level2_flood level4_flood
    boss1 boss2 boss3 boss4 boss5 ending_natural two_player game_over menu_2p)
set(X2_ST_EXPECT_ending_natural "level=1;credits=4")
set(X2_ST_EXPECT_boss1 "level=2")
set(X2_ST_EXPECT_level2_ff "level=2")
set(X2_ST_EXPECT_level3_ff "level=3")
set(X2_ST_EXPECT_level4_ff "level=4")
set(X2_ST_EXPECT_level5_ff "level=5")
set(X2_ST_EXPECT_boss2 "level=2")
set(X2_ST_EXPECT_boss3 "level=3")
set(X2_ST_EXPECT_boss4 "level=4")
set(X2_ST_EXPECT_boss5 "level=5")
set(X2_ST_EXPECT_shop5 "level=5")
set(X2_ST_EXPECT_attract "level=0")
foreach(replay_name ${X2_ST_REPLAYS})
  add_test(NAME replay_st_${replay_name}
    COMMAND ${CMAKE_COMMAND} -DX2RECOMP=$<TARGET_FILE:x2recomp> -DDISK_DIR=${CMAKE_SOURCE_DIR}/generated/st-dev
      -DREPLAYS=${CMAKE_SOURCE_DIR}/tests/replays/st -DNAME=${replay_name} "-DEXPECT=${X2_ST_EXPECT_${replay_name}}"
      -P ${CMAKE_SOURCE_DIR}/tests/st_replay_check.cmake)
  set_tests_properties(replay_st_${replay_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endforeach()

# SDL3 audio output (the headless runtime and --audio-out need no SDL).
if(SDL3_FOUND)
  add_library(x2audio_sdl STATIC runtime/audio/sdl_audio.cpp)
  target_include_directories(x2audio_sdl PUBLIC runtime)
  target_link_libraries(x2audio_sdl PUBLIC SDL3::SDL3)
  x2_warnings(x2audio_sdl)
  target_link_libraries(x2recomp PRIVATE x2audio_sdl) # window sound output
  target_link_libraries(x2st PUBLIC x2audio_sdl)      # the ST window's sound
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

# Chip RAM size (512 KB default, 1 MB selectable): bus mapping and DMA pointers above $7FFFF, synthetic memory only.
add_executable(x2_chip_ram_tests tests/chip_ram_tests.cpp)
target_link_libraries(x2_chip_ram_tests PRIVATE x2gfx)
x2_win_utf8(x2_chip_ram_tests)
add_test(NAME chip_ram COMMAND x2_chip_ram_tests)
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
add_executable(x2_screens_tests tests/screens_tests.cpp)
target_link_libraries(x2_screens_tests PRIVATE x2modern x2st)
x2_win_utf8(x2_screens_tests)
x2_warnings(x2_screens_tests)
add_test(NAME screens COMMAND x2_screens_tests ${CMAKE_BINARY_DIR}/screens_test)
add_executable(x2_pack_tests tests/pack_tests.cpp)
target_link_libraries(x2_pack_tests PRIVATE x2modern x2pack x2cdtv x2runtime)
x2_win_utf8(x2_pack_tests)
add_test(NAME pack COMMAND x2_pack_tests)
add_test(NAME pack_cli COMMAND ${CMAKE_COMMAND} -DX2RECOMP=$<TARGET_FILE:x2recomp> -DWORK=${CMAKE_BINARY_DIR}/pack_cli_work -P ${CMAKE_SOURCE_DIR}/tests/pack_cli.cmake)
add_executable(x2_audio_decode_tests tests/audio/decode_tests.cpp)
target_link_libraries(x2_audio_decode_tests PRIVATE x2audiodec)
x2_warnings(x2_audio_decode_tests)
x2_win_utf8(x2_audio_decode_tests)
# argument: the synthesised tones (tests/audio/fixtures/README.md); X2_AUDIO_FUZZ_ITERATIONS raises the mutation fuzz (default 3000)
add_test(NAME audio_decode COMMAND x2_audio_decode_tests ${CMAKE_SOURCE_DIR}/tests/audio/fixtures)
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

# The CDTV release's runtime services (cdtv/cdtv_game.h): disc-file service, OS start-up skip, dos and CD-audio traps, boot state,
# on a synthetic image (no game data).
add_executable(x2_cdtv_game_runtime_tests tests/cdtv/cdtv_game_runtime_tests.cpp)
target_link_libraries(x2_cdtv_game_runtime_tests PRIVATE x2cdtv x2runtime)
x2_win_utf8(x2_cdtv_game_runtime_tests)
add_test(NAME cdtv_game_runtime COMMAND x2_cdtv_game_runtime_tests ${CMAKE_BINARY_DIR}/cdtv_game_runtime_test)

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
# CDTV game importer: hunk linking, ISO 9660 reading, import/verify/re-import on synthetic discs (no game data). The check of the
# user's own CDTV disc image (cdtv_import_disc, below) is Skipped without it.
add_executable(x2_cdtv_game_tests tests/import/cdtv_game_tests.cpp)
target_link_libraries(x2_cdtv_game_tests PRIVATE x2import)
x2_win_utf8(x2_cdtv_game_tests)
add_test(NAME cdtv_import COMMAND x2_cdtv_game_tests ${CMAKE_BINARY_DIR}/cdtv_import_test)
add_executable(x2_builds_tests tests/import/builds_tests.cpp)
target_link_libraries(x2_builds_tests PRIVATE x2import)
x2_win_utf8(x2_builds_tests)
add_test(NAME builds_import COMMAND x2_builds_tests ${CMAKE_BINARY_DIR}/builds_import_test)
add_executable(x2_disk_image_tests tests/import/disk_image_tests.cpp)
target_link_libraries(x2_disk_image_tests PRIVATE x2import)
add_test(NAME disk_image COMMAND x2_disk_image_tests)
add_executable(x2_flux_tests tests/import/flux_tests.cpp)
target_link_libraries(x2_flux_tests PRIVATE x2import)
add_test(NAME flux COMMAND x2_flux_tests)
add_executable(x2_pdos_tests tests/import/pdos_tests.cpp)
target_link_libraries(x2_pdos_tests PRIVATE x2import)
add_test(NAME pdos COMMAND x2_pdos_tests)
set_tests_properties(pdos PROPERTIES TIMEOUT 900)
add_executable(x2_st_tests tests/import/st_tests.cpp)
target_link_libraries(x2_st_tests PRIVATE x2import)
add_test(NAME st COMMAND x2_st_tests ${CMAKE_BINARY_DIR}/st_test_scratch)
set_tests_properties(st PROPERTIES TIMEOUT 600)
# Local only: analyse every ST disk image (.st, .msa, .stx) under the folder; the Kixx release must import (exit 77 if none).
set(X2_ST_TEST_DIR "" CACHE PATH "A folder of Atari ST disk images (.st, .msa, .stx) for the st_files test")
add_test(NAME st_files COMMAND x2_st_tests ${CMAKE_BINARY_DIR}/st_test_scratch_files --files "${X2_ST_TEST_DIR}")
set_tests_properties(st_files PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
# Local only: import real SCP / FluxEngine dumps of the PDos release found under the folder (exit 77 if none).
set(X2_PDOS_TEST_DIR "" CACHE PATH "A folder of flux dumps (SCP, FluxEngine .flux) of the PDos release for the pdos_files test")
add_test(NAME pdos_files COMMAND x2_pdos_tests --files "${X2_PDOS_TEST_DIR}")
set_tests_properties(pdos_files PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
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
add_test(NAME cdtv_import_disc COMMAND x2_cdtv_game_tests ${CMAKE_BINARY_DIR}/cdtv_import_disc_test --disc "${X2_CDTV_ZIP}")
set_tests_properties(cdtv_import_disc PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
add_test(NAME cdtv_game
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DZIP=${X2_CDTV_ZIP} -DASSETS=${X2_ASSETS_DIR} -DREPLAYS=${CMAKE_SOURCE_DIR}/tests/replays
    -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/cdtv/golden -DOUT=${CMAKE_BINARY_DIR}/cdtv_game -DCDTV_DIR=${X2_CDTV_TEST_DIR}
    -DRECOMP=$<BOOL:${X2_RECOMP}>
    -P ${CMAKE_SOURCE_DIR}/tests/cdtv/cdtv_game.cmake)
set_tests_properties(cdtv_game PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 RUN_SERIAL TRUE)
find_package(Python3 COMPONENTS Interpreter QUIET)
set(X2_CDTV_GAME_ASSETS ${CMAKE_SOURCE_DIR}/generated/assets-cdtv CACHE PATH
  "CDTV release assets (game_0400.bin, cd0/, manifest.txt with build=cdtv)")
# Repository hygiene: no game data, disk images, audio or generated/translated code tracked by git
# (tools/check_no_game_data.py, with its own unit tests). Needs Python 3; the git check is Skipped outside a checkout.
if(Python3_FOUND)
  add_test(NAME check_no_game_data_selftest COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/check_no_game_data.py --self-test)
  # The ST corpus's scripts against their generator, the manifests' shape and the Hatari conversion (no game data, no Hatari).
  add_test(NAME st_replay_scripts COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/trace/test_make_st_replays.py)
  add_test(NAME check_no_game_data COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/check_no_game_data.py --repo ${CMAKE_SOURCE_DIR})
  set_tests_properties(check_no_game_data PROPERTIES SKIP_RETURN_CODE 77)
  # The CDTV build of the recompiler generator (tools/recomp/recomp.py --build cdtv): overlay relocation, image holes, seeds and a
  # synthetic end-to-end generation (Skipped without a C compiler), then the statically relocated overlays against the overlay
  # area of the running game (Skipped without the imported CDTV release).
  add_test(NAME recomp_cdtv_tools COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/recomp/test_cdtv_build.py)
  add_test(NAME recomp_cdtv_overlay_image COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/recomp/test_cdtv_build.py
    --assets ${X2_CDTV_GAME_ASSETS} --exe $<TARGET_FILE:x2recomp>)
  set_tests_properties(recomp_cdtv_tools recomp_cdtv_overlay_image PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
  # The ST build of the generator (--build st): image extraction from a synthetic disk with real (CRC-checked) RNC streams, the tracer's
  # module keys, trace mapping, seeds and a synthetic end-to-end generation (Skipped without a C compiler).
  add_test(NAME recomp_st_tools COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/recomp/test_st_build.py)
  set_tests_properties(recomp_st_tools PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
  # The asset pack authoring tools (tools/assetpack/): PNG codec, atlas packer, slicer, contact sheet, dump merge, skeleton pack.
  add_test(NAME assetpack_tools COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/assetpack/test_assetpack.py)
  set_tests_properties(assetpack_tools PROPERTIES TIMEOUT 300)
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
option(X2_RECOMP "Link the recompiled game code from generated/recomp (disk builds), generated/recomp-cdtv (CDTV release) and generated/recomp-st (Atari ST release); hybrid executor, --interp-only to bypass" OFF)
if(X2_RECOMP)
  # Two independent translations, each generated from its own game image and traces: the disk builds' (tools/recomp/recomp.py)
  # and the CDTV release's (tools/recomp/recomp.py --build cdtv, docs/re-notes/cdtv-recomp.md). A build needs at least one; a
  # game whose translation is not linked runs on the interpreter (an empty module set stands in for it).
  set(X2_RECOMP_DIR ${CMAKE_SOURCE_DIR}/generated/recomp CACHE PATH "Output of tools/recomp/recomp.py (the disk builds)")
  set(X2_RECOMP_CDTV_DIR ${CMAKE_SOURCE_DIR}/generated/recomp-cdtv CACHE PATH "Output of tools/recomp/recomp.py --build cdtv")
  set(X2_RECOMP_ST_DIR ${CMAKE_SOURCE_DIR}/generated/recomp-st CACHE PATH "Output of tools/recomp/recomp.py --build st (docs/re-notes/atari-st-recomp.md)")
  set(X2_RECOMP_SOURCES "")
  set(X2_RECOMP_CDTV_SOURCES "")
  set(X2_RECOMP_ST_SOURCES "")
  set(X2_RECOMP_DISK_FOUND OFF)
  set(X2_RECOMP_CDTV_FOUND OFF)
  set(X2_RECOMP_ST_FOUND OFF)
  if(EXISTS ${X2_RECOMP_DIR}/sources.cmake)
    include(${X2_RECOMP_DIR}/sources.cmake)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${X2_RECOMP_DIR}/sources.cmake)
    set(X2_RECOMP_DISK_FOUND ON)
  endif()
  if(EXISTS ${X2_RECOMP_CDTV_DIR}/sources.cmake)
    include(${X2_RECOMP_CDTV_DIR}/sources.cmake)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${X2_RECOMP_CDTV_DIR}/sources.cmake)
    set(X2_RECOMP_CDTV_FOUND ON)
  endif()
  if(EXISTS ${X2_RECOMP_ST_DIR}/sources.cmake)
    include(${X2_RECOMP_ST_DIR}/sources.cmake)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${X2_RECOMP_ST_DIR}/sources.cmake)
    set(X2_RECOMP_ST_FOUND ON)
  endif()
  if(NOT X2_RECOMP_DISK_FOUND AND NOT X2_RECOMP_CDTV_FOUND AND NOT X2_RECOMP_ST_FOUND)
    message(FATAL_ERROR "X2_RECOMP=ON but none of ${X2_RECOMP_DIR}/sources.cmake, ${X2_RECOMP_CDTV_DIR}/sources.cmake and "
                        "${X2_RECOMP_ST_DIR}/sources.cmake exists: run python3 -I tools/recomp/recomp.py (disk builds) and/or "
                        "python3 -I tools/recomp/recomp.py --build cdtv / --build st")
  endif()
  set(X2_RECOMP_STUBS "")
  if(NOT X2_RECOMP_DISK_FOUND)
    list(APPEND X2_RECOMP_STUBS runtime/recomp/empty_modules_disk.cpp)
  endif()
  if(NOT X2_RECOMP_CDTV_FOUND)
    list(APPEND X2_RECOMP_STUBS runtime/recomp/empty_modules_cdtv.cpp)
  endif()
  if(NOT X2_RECOMP_ST_FOUND)
    list(APPEND X2_RECOMP_STUBS runtime/recomp/empty_modules_st.cpp)
  endif()
  # Generated code is Musashi's handler bodies (C idioms, unused variables): no warnings for it.
  set_source_files_properties(${X2_RECOMP_SOURCES} ${X2_RECOMP_CDTV_SOURCES} ${X2_RECOMP_ST_SOURCES} PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")
  add_library(x2recomp_code STATIC runtime/recomp/executor.cpp ${X2_RECOMP_STUBS} ${X2_RECOMP_SOURCES} ${X2_RECOMP_CDTV_SOURCES} ${X2_RECOMP_ST_SOURCES})
  if(MSVC)
    target_compile_options(x2recomp_code PRIVATE /bigobj) # thousands of functions per generated file
  endif()
  # The sets use different header and symbol names (x2r_, x2rc_ and x2rs_), so one include path list serves all.
  if(X2_RECOMP_DISK_FOUND)
    target_include_directories(x2recomp_code PRIVATE ${X2_RECOMP_DIR})
  endif()
  if(X2_RECOMP_CDTV_FOUND)
    target_include_directories(x2recomp_code PRIVATE ${X2_RECOMP_CDTV_DIR})
  endif()
  if(X2_RECOMP_ST_FOUND)
    target_include_directories(x2recomp_code PRIVATE ${X2_RECOMP_ST_DIR})
  endif()
  target_link_libraries(x2recomp_code PUBLIC x2runtime)
  target_compile_definitions(x2recomp PRIVATE X2_HAVE_RECOMP=1)
  target_link_libraries(x2recomp PRIVATE x2recomp_code)

  # Per-function differential test against the interpreter (needs the game images the code was generated from).
  add_executable(x2_recomp_difftest tests/recomp/difftest.cpp tests/recomp/difftest_st.cpp)
  target_link_libraries(x2_recomp_difftest PRIVATE x2recomp_code x2import)
  x2_win_utf8(x2_recomp_difftest)
  add_test(NAME recomp_difftest COMMAND x2_recomp_difftest --assets ${X2_ASSETS_DIR})
  set_tests_properties(recomp_difftest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
  # The disk builds' equivalence tests need their translation (a CDTV-only recompiled build has none).
  if(X2_RECOMP_DISK_FOUND)
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
# The Rob Northen PDos release (docs/re-notes/pdos-v5.md). Only the front-end replay is recorded: a replay that starts a level loads
# one of the level files, and the only dump of this release read so far has unreadable sectors inside the first three of them. Assets:
# tools/variants/write_assets.py pdos-v5 DISK.bin DIR (--allow-damaged writes development assets from an incomplete disk).
set(X2_PDOS_ASSETS ${CMAKE_SOURCE_DIR}/generated/assets-pdos-v5 CACHE PATH
  "PDos release assets (game_0400.bin, disc2.bin, manifest.txt with build=pdos-v5)")
add_test(NAME build_pdos-v5_attract
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_PDOS_ASSETS}
    -DREPLAY=${CMAKE_SOURCE_DIR}/tests/replays/attract.txt
    -DHASHES=${CMAKE_SOURCE_DIR}/tests/replays/pdos-v5/attract.hashes
    -P ${CMAKE_SOURCE_DIR}/tests/replay_check.cmake)
set_tests_properties(build_pdos-v5_attract PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
# The CDTV release (docs/re-notes/cdtv-boot.md): the pre-linked X2 and the disc's files in cd0/, imported by `x2recomp --import` of the
# disc image; our own recorded hash manifests in tests/replays/cdtv/ (its front end differs from the disk build's, so the scripts do
# too: tools/trace/make_cdtv_replays.py). Skipped without the assets.
set(X2_CDTV_GAME_ASSETS ${CMAKE_SOURCE_DIR}/generated/assets-cdtv CACHE PATH
  "CDTV release assets (game_0400.bin, cd0/, manifest.txt with build=cdtv)")
# The corpus (docs/re-notes/cdtv-replays.md): title and guide pages, menus, all five levels, the level-1 boss defeated and the others
# reached, the mid-level and level-5 shops, floods, the natural ending, game over, two players.
set(X2_CDTV_REPLAYS attract menus menu_2p level1_play level1_special level1_ff level1_flood shop shop5
  level2 level2_flood level3 level4 level4_flood level4_boss level5 boss1 boss2 boss3 two_player game_over ending ending_natural)
foreach(replay_name ${X2_CDTV_REPLAYS})
  add_test(NAME cdtv_replay_${replay_name}
    COMMAND ${CMAKE_COMMAND}
      -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_CDTV_GAME_ASSETS}
      -DREPLAY=${CMAKE_SOURCE_DIR}/tests/replays/cdtv/${replay_name}.txt
      -DHASHES=${CMAKE_SOURCE_DIR}/tests/replays/cdtv/${replay_name}.hashes
      -P ${CMAKE_SOURCE_DIR}/tests/replay_check.cmake)
  set_tests_properties(cdtv_replay_${replay_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()
# The modernisation layer on the CDTV release (docs/re-notes/cdtv-modern.md): the same checks as the disk build's, with the CDTV
# assets and the frame window of its level 1 (level 1 loads at frame 2105; gameplay from about 2400). Skipped without the assets.
#  - interpolation endpoints exact on every replay of the corpus;
#  - modern_cdtv_game: endpoints, the captured picture equal to the rendered frame during gameplay, and (recompiled builds) the draw
#    capture identical on the translated executor and --interp-only;
#  - Faithful pictures identical to the rendered frames, the surround's centre exact, the asset dump reproducing the pictures,
#    golden hashes of interpolated and surround pictures (tests/golden/modern_cdtv_*.txt).
set(X2_CDTV_LEVEL1 ${CMAKE_SOURCE_DIR}/tests/replays/cdtv/level1_play.txt)
foreach(replay_name ${X2_CDTV_REPLAYS})
  set(min_pairs 1)
  if(replay_name STREQUAL "attract" OR replay_name STREQUAL "menus") # title and menu pages only: nothing to interpolate
    set(min_pairs 0)
  endif()
  add_test(NAME modern_cdtv_check_${replay_name}
    COMMAND ${CMAKE_COMMAND}
      -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_CDTV_GAME_ASSETS}
      -DREPLAY=${CMAKE_SOURCE_DIR}/tests/replays/cdtv/${replay_name}.txt -DMIN_PAIRS=${min_pairs}
      -P ${CMAKE_SOURCE_DIR}/tests/modern_check.cmake)
  set_tests_properties(modern_cdtv_check_${replay_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endforeach()
set(X2_CDTV_RECOMP_ON 0)
if(X2_RECOMP AND X2_RECOMP_CDTV_FOUND)
  set(X2_CDTV_RECOMP_ON 1)
endif()
add_test(NAME modern_cdtv_game
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_CDTV_GAME_ASSETS} -DOUT=${CMAKE_BINARY_DIR}/modern_cdtv_game
    -DREPLAY=${X2_CDTV_LEVEL1} -DRECOMP=${X2_CDTV_RECOMP_ON} -DCHECK_FRAMES=3300 -DMIN_PAIRS=200 -DLOG_FROM=2900 -DLOG_TO=3200
    -DGAMEPLAY_FROM=2400
    -P ${CMAKE_SOURCE_DIR}/tests/modern_game.cmake)
add_test(NAME modern_cdtv_faithful
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_CDTV_GAME_ASSETS} -DREPLAY=${X2_CDTV_LEVEL1} -DFRAMES=3060 -DFROM=2700 -DTO=3000
    -P ${CMAKE_SOURCE_DIR}/tests/modern_faithful.cmake)
add_test(NAME modern_cdtv_wide
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_CDTV_GAME_ASSETS} -DREPLAY=${X2_CDTV_LEVEL1} -DFRAMES=3060 -DFROM=2700 -DTO=3000
    -DCHECK_FRAMES=3300 -DMIN_PAIRS=200
    -P ${CMAKE_SOURCE_DIR}/tests/modern_wide.cmake)
add_test(NAME modern_cdtv_upgrade
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_CDTV_GAME_ASSETS} -DOUT=${CMAKE_BINARY_DIR}/modern_cdtv_upgrade
    -DREPLAY=${X2_CDTV_LEVEL1} -DRECOMP=${X2_CDTV_RECOMP_ON} -DFRAMES=3060 -DFROM=2700 -DTO=3000 -DREC_FROM=2700
    -P ${CMAKE_SOURCE_DIR}/tests/modern_upgrade.cmake)
add_test(NAME modern_cdtv_midpoints
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_CDTV_GAME_ASSETS} -DREPLAYS=${CMAKE_SOURCE_DIR}/tests/replays/cdtv
    -DOUT=${CMAKE_BINARY_DIR}/modern_cdtv_midpoints -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/golden/modern_cdtv_midpoints.txt
    -P ${CMAKE_SOURCE_DIR}/tests/modern_midpoints.cmake)
add_test(NAME modern_cdtv_wide_golden
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_CDTV_GAME_ASSETS} -DREPLAYS=${CMAKE_SOURCE_DIR}/tests/replays/cdtv
    -DOUT=${CMAKE_BINARY_DIR}/modern_cdtv_wide_golden -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/golden/modern_cdtv_wide.txt
    -P ${CMAKE_SOURCE_DIR}/tests/modern_midpoints.cmake)
set_tests_properties(modern_cdtv_game modern_cdtv_faithful modern_cdtv_wide modern_cdtv_upgrade modern_cdtv_midpoints
  modern_cdtv_wide_golden PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
# The CDTV release's CD audio (docs/re-notes/cdtv-audio.md): the same replays with the disc's audio tracks mixed in (--music cdtv):
# frames unchanged, recorded audio hashes (tests/replays/cdtv/NAME.cd.hashes) and CD requests (NAME.cues); without CD tracks the music is
# silent and the hashes are the CD-free ones. Skipped without the imported release or the disc (the audio import, about 680 MB, goes to
# X2_CDTV_TEST_DIR and is shared with cdtv_game).
foreach(replay_name menus level1_play level3 shop5)
  add_test(NAME cdtv_audio_${replay_name}
    COMMAND ${CMAKE_COMMAND}
      -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_CDTV_GAME_ASSETS} -DZIP=${X2_CDTV_ZIP} -DCDTV_DIR=${X2_CDTV_TEST_DIR}
      -DREPLAYS=${CMAKE_SOURCE_DIR}/tests/replays/cdtv -DNAME=${replay_name} -DOUT=${CMAKE_BINARY_DIR}/cdtv_audio_${replay_name}
      -P ${CMAKE_SOURCE_DIR}/tests/cdtv/cdtv_release_audio.cmake)
  set_tests_properties(cdtv_audio_${replay_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 RESOURCE_LOCK cdtv_audio_import)
endforeach()
# The CDTV release on the translated executor (docs/re-notes/cdtv-recomp.md; needs -DX2_RECOMP=ON and generated/recomp-cdtv): every replay
# of its corpus, interpreter (--interp-only) against hybrid: frame and audio hashes every frame, register logs, final chip RAM, and the
# share of executed instructions that ran as translated code. The recorded manifests above are checked by the cdtv_replay_* tests in
# either build (a recompiled build runs them on the translated code).
if(X2_RECOMP AND X2_RECOMP_CDTV_FOUND)
  set(X2_CDTV_MIN_COVERAGE 90 CACHE STRING "Percent of executed instructions that must run as translated code in recomp_cdtv_*")
  foreach(replay_name ${X2_CDTV_REPLAYS})
    add_test(NAME recomp_cdtv_${replay_name}
      COMMAND ${CMAKE_COMMAND}
        -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_CDTV_GAME_ASSETS}
        -DREPLAY=${CMAKE_SOURCE_DIR}/tests/replays/cdtv/${replay_name}.txt
        -DOUT=${CMAKE_BINARY_DIR}/recomp_cdtv/${replay_name} -DMIN_COVERAGE=${X2_CDTV_MIN_COVERAGE}
        -P ${CMAKE_SOURCE_DIR}/tests/recomp/replay_equivalence.cmake)
    set_tests_properties(recomp_cdtv_${replay_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
  endforeach()
  # Every translated function of the CDTV release (and random mid-function entries) from random machine states, interpreter against
  # hybrid: CPU context, all of chip RAM, custom chip and CIA state, register-write log, cycles and the data bus accesses.
  add_test(NAME recomp_cdtv_difftest COMMAND x2_recomp_difftest --game cdtv --assets ${X2_CDTV_GAME_ASSETS})
  set_tests_properties(recomp_cdtv_difftest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
endif()
# The Atari ST release on the translated executor (docs/re-notes/atari-st-recomp.md; needs -DX2_RECOMP=ON and generated/recomp-st): every
# ST script (tests/replays/st and tests/recomp/st_scenarios), interpreter (--interp-only) against hybrid: frame and audio hashes and a
# digest of RAM, registers and cycle counter at every frame, the frame logs, final RAM, and the share of instructions run translated.
set(X2_ST_GAME_ASSETS ${CMAKE_SOURCE_DIR}/generated/assets-st CACHE PATH "The imported Atari ST (Kixx) release (disk.bin, manifest.txt)")
# Screens and the status panel from asset packs (docs/re-notes/asset-packs-screens.md), on every game build with data: the replay's own
# screens and panel cells written by --dump-assets, loaded back as a pack, give exactly the frames the game rendered (except in fades,
# fitted to one colour level, and where the panel's palette is not a fade of the pack's), at several canvas scales; deterministic; the
# Faithful switch bypasses the pack. The scratch packs live in the build tree. Skipped without the assets (exit 77).
function(x2_screens_test name game assets replay min_screens panel scales)
  add_test(NAME screens_${name}
    COMMAND ${CMAKE_COMMAND}
      -DX2RECOMP=$<TARGET_FILE:x2recomp> -DGAME=${game} -DASSETS=${assets} -DREPLAY=${replay}
      -DOUT=${CMAKE_BINARY_DIR}/screens_data/${name} -DMIN_SCREENS=${min_screens} -DPANEL=${panel} "-DSCALES=${scales}"
      ${ARGN}
      -P ${CMAKE_SOURCE_DIR}/tests/modern_screens.cmake)
  set_tests_properties(screens_${name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endfunction()
set(X2_REPLAYS ${CMAKE_SOURCE_DIR}/tests/replays)
x2_screens_test(disk_shop disk ${X2_ASSETS_DIR} ${X2_REPLAYS}/shop.txt 4 1 "2;4" -DFAITHFUL_FROM=2000 -DFAITHFUL_TO=2300)
x2_screens_test(disk_attract disk ${X2_ASSETS_DIR} ${X2_REPLAYS}/attract.txt 5 0 "3")
x2_screens_test(disk_ending disk ${X2_ASSETS_DIR} ${X2_REPLAYS}/ending_natural.txt 5 1 "2")
x2_screens_test(retail2874_attract disk ${X2_RETAIL_2874_ASSETS} ${X2_REPLAYS}/attract.txt 5 0 "2")
x2_screens_test(retail0297_attract disk ${X2_RETAIL_0297_ASSETS} ${X2_REPLAYS}/attract.txt 5 0 "2")
x2_screens_test(power-pack-2234_shop disk ${X2_POWER_PACK_ASSETS} ${X2_REPLAYS}/shop.txt 4 1 "2")
x2_screens_test(compilation-1762_shop disk ${X2_COMPILATION_1762_ASSETS} ${X2_REPLAYS}/shop.txt 4 1 "2")
x2_screens_test(pdos-v5_attract disk ${X2_PDOS_ASSETS} ${X2_REPLAYS}/attract.txt 5 0 "2")
x2_screens_test(cdtv_shop cdtv ${X2_CDTV_GAME_ASSETS} ${X2_REPLAYS}/cdtv/shop.txt 4 1 "2;4" -DFAITHFUL_FROM=2400 -DFAITHFUL_TO=2700)
x2_screens_test(cdtv_menus cdtv ${X2_CDTV_GAME_ASSETS} ${X2_REPLAYS}/cdtv/menus.txt 2 0 "2")
x2_screens_test(st_shop st ${X2_ST_GAME_ASSETS} ${X2_REPLAYS}/st/shop.txt 6 1 "2;4")
x2_screens_test(st_attract st ${X2_ST_GAME_ASSETS} ${X2_REPLAYS}/st/attract.txt 5 0 "3")
if(X2_RECOMP AND X2_RECOMP_ST_FOUND)
  set(X2_ST_MIN_COVERAGE 90 CACHE STRING "Percent of executed instructions that must run as translated code in recomp_st_*")
  file(GLOB X2_ST_RECOMP_SCRIPTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/replays/st/*.txt ${CMAKE_SOURCE_DIR}/tests/recomp/st_scenarios/*.txt)
  foreach(script IN LISTS X2_ST_RECOMP_SCRIPTS)
    get_filename_component(script_name ${script} NAME_WE)
    # tests/recomp/st_scenarios and tests/replays/st share some names (attract, level1_play, game_over): the scenarios get a prefix
    if(script MATCHES "/st_scenarios/")
      set(script_name scn_${script_name})
    endif()
    add_test(NAME recomp_st_${script_name}
      COMMAND ${CMAKE_COMMAND}
        -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ST_GAME_ASSETS} -DREPLAY=${script}
        -DOUT=${CMAKE_BINARY_DIR}/recomp_st/${script_name} -DMIN_COVERAGE=${X2_ST_MIN_COVERAGE}
        -P ${CMAKE_SOURCE_DIR}/tests/recomp/st_equivalence.cmake)
    set_tests_properties(recomp_st_${script_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
  endforeach()
  add_test(NAME recomp_st_difftest COMMAND x2_recomp_difftest --game st --assets ${X2_ST_GAME_ASSETS})
  set_tests_properties(recomp_st_difftest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
endif()
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

# The Power Pack, the 1762 compilation, the one-disk crunches and the cracks that need their own recipe, with real disk
# images: tests/builds_import.cmake. Each group is skipped when its images are not there.
set(X2_SOURCES_DIR "${CMAKE_SOURCE_DIR}/generated/sources" CACHE PATH
  "Folder holding Xenon-2-Megablast_Amiga_EN/ (the extracted Amiga_EN archive; docs/re-notes/sources.md)")
add_test(NAME builds_sources_import
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DSRC=${X2_EXTRA_SOURCES_DIR} -DSRC2=${X2_SOURCES_DIR} -DDISC2=${X2_ASSETS_DIR}/disc2.bin
    -DREPLAYS=${CMAKE_SOURCE_DIR}/tests/replays -DOUT=${CMAKE_BINARY_DIR}/builds_sources_import
    -P ${CMAKE_SOURCE_DIR}/tests/builds_import.cmake)
set_tests_properties(builds_sources_import PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)

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

# The disk build in the 1 MB chip RAM model (--chip-ram 1024) must reproduce the faithful frame and audio hashes: the game
# never uses the 512 KB mirror, so only the memory size differs (docs/re-notes/cdtv-runtime-gap.md G1). Same manifests.
foreach(replay_name level1_play boss1)
  add_test(NAME chip_ram_1m_${replay_name}
    COMMAND ${CMAKE_COMMAND}
      -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR}
      -DREPLAY=${CMAKE_SOURCE_DIR}/tests/replays/${replay_name}.txt
      -DHASHES=${CMAKE_SOURCE_DIR}/tests/replays/${replay_name}.hashes "-DEXTRA=--chip-ram;1024"
      -P ${CMAKE_SOURCE_DIR}/tests/replay_check.cmake)
  set_tests_properties(chip_ram_1m_${replay_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
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
  docs/PORTING.md docs/DUMPING.md DESTINATION ${X2_DOC_DIR})
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
