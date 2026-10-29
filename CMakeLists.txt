cmake_minimum_required(VERSION 3.20)
project(xenon2_recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# --- Musashi 68000 core (vendor/musashi). m68kops.c/h are generated from m68k_in.c by m68kmake. ---
set(MUSASHI_DIR ${CMAKE_SOURCE_DIR}/vendor/musashi)
set(MUSASHI_GEN ${CMAKE_BINARY_DIR}/musashi_gen)
add_executable(m68kmake ${MUSASHI_DIR}/m68kmake.c)
add_custom_command(
  OUTPUT ${MUSASHI_GEN}/m68kops.c ${MUSASHI_GEN}/m68kops.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSASHI_GEN}
  COMMAND m68kmake ${MUSASHI_GEN} ${MUSASHI_DIR}/m68k_in.c
  DEPENDS m68kmake ${MUSASHI_DIR}/m68k_in.c
  COMMENT "Generating Musashi opcode tables")

add_library(musashi STATIC
  ${MUSASHI_DIR}/m68kcpu.c
  ${MUSASHI_DIR}/m68kdasm.c
  ${MUSASHI_DIR}/softfloat/softfloat.c
  ${MUSASHI_GEN}/m68kops.c)
target_include_directories(musashi PUBLIC ${MUSASHI_DIR} ${MUSASHI_GEN} ${CMAKE_SOURCE_DIR}/runtime)
target_compile_definitions(musashi PUBLIC MUSASHI_CNF="x2_m68kconf.h")
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
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
  runtime/trace/tracer.cpp
  runtime/audio/audio_cues.cpp
  runtime/audio/paula.cpp
  runtime/audio/wav_writer.cpp)
target_include_directories(x2runtime PUBLIC runtime)
target_link_libraries(x2runtime PUBLIC musashi)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(x2runtime PRIVATE -Wall -Wextra)
endif()

add_executable(x2recomp runtime/main.cpp)
target_link_libraries(x2recomp PRIVATE x2runtime)

# --- video (scanline Denise/Agnus model, screenshots; no platform dependencies) ---
add_library(x2gfx STATIC runtime/gfx/video.cpp runtime/gfx/image_io.cpp)
target_link_libraries(x2gfx PUBLIC x2runtime)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(x2gfx PRIVATE -Wall -Wextra)
endif()
target_link_libraries(x2recomp PRIVATE x2gfx)

# --- modernisation layer (optional presentation features, off by default; docs/re-notes/modernisation-design.md) ---
add_library(x2modern STATIC runtime/modern/draw_capture.cpp runtime/modern/interp.cpp runtime/modern/present.cpp)
target_link_libraries(x2modern PUBLIC x2runtime)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(x2modern PRIVATE -Wall -Wextra)
endif()
target_link_libraries(x2recomp PRIVATE x2modern)

# --- disk image import (ADF/ADZ/ZIP -> game_0400.bin + disc2.bin; runtime/import, inflate by vendor/puff) ---
add_library(x2import STATIC runtime/import/importer.cpp runtime/import/sha1.cpp vendor/puff/puff.c)
target_include_directories(x2import PUBLIC runtime PRIVATE vendor/puff)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(x2import PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)
endif()
target_link_libraries(x2recomp PRIVATE x2import)

# --- SDL3 window and input (optional: without SDL3 only --headless is available) ---
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  add_library(x2host STATIC runtime/gfx/sdl_display.cpp runtime/gfx/sdl_filters.cpp runtime/input/input.cpp
    runtime/import/import_screen.cpp)
  target_link_libraries(x2host PUBLIC x2gfx x2import SDL3::SDL3)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(x2host PRIVATE -Wall -Wextra)
  endif()
  target_link_libraries(x2recomp PRIVATE x2host)
  target_compile_definitions(x2recomp PRIVATE X2_HAVE_SDL=1)
else()
  message(STATUS "SDL3 not found: building x2recomp without a window (headless only)")
endif()

# --- SDL3 audio output (optional: the headless runtime and --audio-out need no SDL) ---
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  add_library(x2audio_sdl STATIC runtime/audio/sdl_audio.cpp)
  target_include_directories(x2audio_sdl PUBLIC runtime)
  target_link_libraries(x2audio_sdl PUBLIC SDL3::SDL3)
  target_link_libraries(x2recomp PRIVATE x2audio_sdl) # window sound output
endif()

# --- tests ---
enable_testing()
add_executable(x2_smoke_tests tests/smoke_tests.cpp)
target_link_libraries(x2_smoke_tests PRIVATE x2runtime)
add_test(NAME smoke COMMAND x2_smoke_tests)
add_executable(x2_timing_tests tests/timing_tests.cpp)
target_link_libraries(x2_timing_tests PRIVATE x2runtime)
add_test(NAME timing COMMAND x2_timing_tests)
add_executable(x2_video_tests tests/video_tests.cpp)
target_link_libraries(x2_video_tests PRIVATE x2gfx)
add_test(NAME video COMMAND x2_video_tests)
add_executable(x2_modern_tests tests/modern_tests.cpp)
target_link_libraries(x2_modern_tests PRIVATE x2modern)
add_test(NAME modern COMMAND x2_modern_tests)
if(TARGET x2host)
  add_executable(x2_input_tests tests/input_tests.cpp)
  target_link_libraries(x2_input_tests PRIVATE x2host)
  add_test(NAME input COMMAND x2_input_tests)
endif()
add_executable(x2_paula_tests tests/audio/paula_tests.cpp)
target_link_libraries(x2_paula_tests PRIVATE x2runtime)
add_test(NAME paula COMMAND x2_paula_tests)
add_executable(x2_persist_tests tests/persist_tests.cpp)
target_link_libraries(x2_persist_tests PRIVATE x2runtime)
add_test(NAME persist COMMAND x2_persist_tests ${CMAKE_BINARY_DIR}/persist_test)
if(TARGET x2audio_sdl)
  add_executable(x2_sdl_audio_smoke tests/audio/sdl_audio_smoke.cpp)
  target_link_libraries(x2_sdl_audio_smoke PRIVATE x2audio_sdl)
  add_test(NAME sdl_audio_smoke COMMAND x2_sdl_audio_smoke)
endif()

# Importer: synthetic ADF/ADZ/ZIP inputs (no game data), then the user's own Xenon2.zip / Amiga_EN zip if present
# (that part is reported as Skipped without them).
add_executable(x2_import_tests tests/import/import_tests.cpp)
target_link_libraries(x2_import_tests PRIVATE x2import)
add_test(NAME import COMMAND x2_import_tests ${CMAKE_BINARY_DIR}/import_test)
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
find_package(Python3 COMPONENTS Interpreter QUIET)
add_test(NAME game_audio
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DOUT=${CMAKE_BINARY_DIR}/game_audio
    -DPYTHON=${Python3_EXECUTABLE} -DCHECK=${CMAKE_SOURCE_DIR}/tests/audio/check_wav.py
    -P ${CMAKE_SOURCE_DIR}/tests/audio/game_audio.cmake)
set_tests_properties(game_audio PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
# --data-dir moves settings.cfg, input.cfg and highscores.bin too; headless runs write none of them.
if(SDL3_FOUND)
  add_test(NAME data_dir
    COMMAND ${CMAKE_COMMAND}
      -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DOUT=${CMAKE_BINARY_DIR}/data_dir_test
      -P ${CMAKE_SOURCE_DIR}/tests/data_dir.cmake)
  set_tests_properties(data_dir PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
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
  set_source_files_properties(${X2_RECOMP_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
  add_library(x2recomp_code STATIC runtime/recomp/executor.cpp ${X2_RECOMP_SOURCES})
  target_include_directories(x2recomp_code PRIVATE ${X2_RECOMP_DIR})
  target_link_libraries(x2recomp_code PUBLIC x2runtime)
  target_compile_definitions(x2recomp PRIVATE X2_HAVE_RECOMP=1)
  target_link_libraries(x2recomp PRIVATE x2recomp_code)

  # Per-function differential test against the interpreter (needs the game images the code was generated from).
  add_executable(x2_recomp_difftest tests/recomp/difftest.cpp)
  target_link_libraries(x2_recomp_difftest PRIVATE x2recomp_code)
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
