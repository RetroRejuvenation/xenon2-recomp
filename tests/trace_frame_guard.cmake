# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# --trace-frame without a place to write the trace (no --dump-dir, or a folder that does not exist) reports it and carries on instead of
# writing through a null FILE*. Exits 77 (CTest "Skipped") without the user's own disk-release assets.
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<dir> -DOUT=<dir> -P trace_frame_guard.cmake
if(NOT EXISTS "${ASSETS}/game_0400.bin" OR NOT EXISTS "${ASSETS}/disc2.bin")
  message("SKIPPED: no disk game data in ${ASSETS}")
  cmake_language(EXIT 77)
endif()
file(REMOVE_RECURSE "${OUT}")
set(ENV{HOME} "${OUT}/home")
set(ENV{XDG_DATA_HOME} "${OUT}/home/xdg")
set(ENV{APPDATA} "${OUT}/home/appdata")
set(common --headless --game disk --assets "${ASSETS}" --no-audio --no-persist --frames 3 --trace-frame 1 --data-dir "${OUT}/data")

execute_process(COMMAND "${X2RECOMP}" ${common}
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT err MATCHES "--trace-frame needs --dump-dir")
  message(FATAL_ERROR "no --dump-dir: exit ${rc}\n${out}${err}")
endif()

execute_process(COMMAND "${X2RECOMP}" ${common} --dump-dir "${OUT}/no/such/folder"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT err MATCHES "cannot write")
  message(FATAL_ERROR "unwritable --dump-dir: exit ${rc}\n${out}${err}")
endif()

file(MAKE_DIRECTORY "${OUT}/dump")
execute_process(COMMAND "${X2RECOMP}" ${common} --dump-dir "${OUT}/dump"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}/dump/trace_1.txt")
  message(FATAL_ERROR "normal --trace-frame: exit ${rc}\n${out}${err}")
endif()
message("trace-frame guard ok")
