# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Interpolation endpoint exactness on one replay (docs/re-notes/modernisation-design.md): with far-layer smoothing
# off, every interpolable step pair re-renders exactly at t = 0 and t = 1. Exits 77 (CTest "Skipped") without
# the user's assets.
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<dir> -DREPLAY=<script> [-DMIN_PAIRS=N] -P modern_check.cmake
if(NOT EXISTS "${ASSETS}/game_0400.bin" OR NOT EXISTS "${ASSETS}/disc2.bin")
  message("SKIPPED: no game data in ${ASSETS}")
  cmake_language(EXIT 77)
endif()
if(NOT DEFINED MIN_PAIRS)
  set(MIN_PAIRS 1)
endif()
execute_process(
  COMMAND "${X2RECOMP}" --headless --assets "${ASSETS}" --no-reglog --report-every 0 --input-script "${REPLAY}"
          --modern-check
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "x2recomp exited with ${rc}: ${err}")
endif()
if(NOT out MATCHES "modern check: ([0-9]+) interpolable step pairs; t=0 exact ([0-9]+), t=1 exact ([0-9]+)")
  message(FATAL_ERROR "no modern check summary\n${out}${err}")
endif()
set(pairs ${CMAKE_MATCH_1})
if(pairs LESS MIN_PAIRS OR NOT CMAKE_MATCH_2 EQUAL pairs OR NOT CMAKE_MATCH_3 EQUAL pairs)
  message(FATAL_ERROR "interpolation endpoints not exact (at least ${MIN_PAIRS} pairs expected): ${CMAKE_MATCH_0}")
endif()
message("${CMAKE_MATCH_0}")
