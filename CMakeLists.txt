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

# --- SDL3 window and input (optional: without SDL3 only --headless is available) ---
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  add_library(x2host STATIC runtime/gfx/sdl_display.cpp runtime/input/input.cpp)
  target_link_libraries(x2host PUBLIC x2gfx SDL3::SDL3)
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
add_executable(x2_video_tests tests/video_tests.cpp)
target_link_libraries(x2_video_tests PRIVATE x2gfx)
add_test(NAME video COMMAND x2_video_tests)
if(TARGET x2host)
  add_executable(x2_input_tests tests/input_tests.cpp)
  target_link_libraries(x2_input_tests PRIVATE x2host)
  add_test(NAME input COMMAND x2_input_tests)
endif()
add_executable(x2_paula_tests tests/audio/paula_tests.cpp)
target_link_libraries(x2_paula_tests PRIVATE x2runtime)
add_test(NAME paula COMMAND x2_paula_tests)
if(TARGET x2audio_sdl)
  add_executable(x2_sdl_audio_smoke tests/audio/sdl_audio_smoke.cpp)
  target_link_libraries(x2_sdl_audio_smoke PRIVATE x2audio_sdl)
  add_test(NAME sdl_audio_smoke COMMAND x2_sdl_audio_smoke)
endif()

# Needs user-supplied game data (tools/prep_assets.py). Exit code 77 = skipped, reported as "Skipped" by CTest.
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
# Golden frame hashes of the scripted run (tests/golden/game_frames.txt). Skipped without game data.
add_test(NAME golden_frames
  COMMAND ${CMAKE_COMMAND}
    -DX2RECOMP=$<TARGET_FILE:x2recomp> -DASSETS=${X2_ASSETS_DIR} -DOUT=${CMAKE_BINARY_DIR}/golden_frames
    -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/golden/game_frames.txt
    -P ${CMAKE_SOURCE_DIR}/tests/golden_frames.cmake)
set_tests_properties(golden_frames PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
