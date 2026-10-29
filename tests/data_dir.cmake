# --data-dir: in window mode settings.cfg, input.cfg and highscores.bin follow it (one resolver for the per-user
# data directory); a headless run (as every test and replay) reads and writes none of them. Window mode runs on
# SDL's dummy video driver. Exits 77 (CTest "Skipped") when the user's assets are absent.
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<dir> -DOUT=<dir> -P data_dir.cmake   (needs CMake >= 3.29)
if(NOT EXISTS "${ASSETS}/game_0400.bin" OR NOT EXISTS "${ASSETS}/disc2.bin")
  message("SKIPPED: no game data in ${ASSETS}; run tools/prep_assets.py with your own ADFs first")
  cmake_language(EXIT 77)
endif()
file(REMOVE_RECURSE "${OUT}")
# Any fallback to the real per-user directory would land in this HOME instead of the user's.
set(ENV{HOME} "${OUT}/home")
set(ENV{XDG_DATA_HOME} "${OUT}/home/xdg")
set(ENV{APPDATA} "${OUT}/home/appdata") # Windows
# SDL3's names, and SDL2's (still read by SDL3).
set(ENV{SDL_VIDEO_DRIVER} dummy)
set(ENV{SDL_AUDIO_DRIVER} dummy)
set(ENV{SDL_VIDEODRIVER} dummy)
set(ENV{SDL_AUDIODRIVER} dummy)

execute_process(
  COMMAND "${X2RECOMP}" --data-dir "${OUT}/data" --assets "${ASSETS}" --frames 20 --no-audio
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "x2recomp (window) exited with ${rc}")
endif()
foreach(f settings.cfg input.cfg)
  if(NOT EXISTS "${OUT}/data/${f}")
    message(FATAL_ERROR "window mode did not keep ${f} in --data-dir")
  endif()
endforeach()
if(NOT out MATCHES "high scores: ${OUT}/data/highscores.bin")
  message(FATAL_ERROR "the high-score file is not in --data-dir")
endif()

execute_process(
  COMMAND "${X2RECOMP}" --headless --data-dir "${OUT}/headless" --assets "${ASSETS}" --frames 20 --no-reglog
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "x2recomp (headless) exited with ${rc}")
endif()
file(GLOB_RECURSE written "${OUT}/headless/*" "${OUT}/home/*")
if(written OR EXISTS "${OUT}/headless")
  message(FATAL_ERROR "a headless run wrote per-user files: ${written}")
endif()
message("data_dir: window files in --data-dir, headless wrote nothing")
