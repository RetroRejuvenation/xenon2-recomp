# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# The in-game half of the launcher's flow, on the user's own assets for one release (a window run on SDL's dummy drivers, the arguments the launcher
# hands to the game: --game): the game starts, F1 opens the menu, the menu is walked to Quit with the keyboard alone, the question is answered, and
# the run ends at once instead of running to its frame limit. Exits 77 (CTest "Skipped") without the assets. The launcher's own half (import,
# Play, the result it returns) is the launcher_sdl test.
# Usage: cmake -DX2RECOMP=<exe> -DGAME=disk|cdtv|st -DASSETS=<dir> -DOUT=<dir> -P gui_flow_check.cmake
if(GAME STREQUAL "st")
  set(need "${ASSETS}/disk.bin")
else()
  set(need "${ASSETS}/game_0400.bin")
endif()
if(NOT EXISTS "${need}")
  message("SKIPPED: no ${GAME} game data in ${ASSETS}")
  cmake_language(EXIT 77)
endif()
if(GAME STREQUAL "disk" AND NOT EXISTS "${ASSETS}/disc2.bin")
  message("SKIPPED: no disk game data in ${ASSETS}")
  cmake_language(EXIT 77)
endif()
file(REMOVE_RECURSE "${OUT}")
set(ENV{HOME} "${OUT}/home")
set(ENV{XDG_DATA_HOME} "${OUT}/home/xdg")
set(ENV{APPDATA} "${OUT}/home/appdata")
set(ENV{SDL_VIDEO_DRIVER} dummy)
set(ENV{SDL_AUDIO_DRIVER} dummy)
set(ENV{SDL_VIDEODRIVER} dummy)
set(ENV{SDL_AUDIODRIVER} dummy)

set(common --game ${GAME} --assets "${ASSETS}" --no-audio --no-persist --frames 4000 --hash-at 3900 --data-dir "${OUT}/data")
if(GAME STREQUAL "st")
  list(APPEND common --st-fdc-speed 8)
endif()
# F1 opens the menu; ten Down presses reach Quit (Resume, Faithful, Display, Audio, Controls, Asset packs, Assists, Reset, Back to launcher, Quit);
# Return asks, Left moves from Cancel to the confirmation, Return answers.
set(script "60:F1,Down,Down,Down,Down,Down,Down,Down,Down,Down,Return,Left,Return")
execute_process(COMMAND "${X2RECOMP}" ${common} --menu-script "${script}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 200)
message("${out}${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the run exited with ${rc} (a timeout means the menu's Quit did not end it)")
endif()
if(NOT out MATCHES "menu: open")
  message(FATAL_ERROR "the menu never opened")
endif()
if(out MATCHES "framehash 3900")
  message(FATAL_ERROR "the run went on to its frame limit: Quit in the menu did not end it")
endif()
if(EXISTS "${OUT}/data/x2recomp.log")
  message("note: a log file was written next to the data")
endif()
message("gui_flow (${GAME}): the menu opened, Quit in it ended the run before the frame limit")
