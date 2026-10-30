# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# CTest driver for the modernisation spike on the real game (docs/re-notes/modernisation-design.md). Exits 77
# (CTest "Skipped") when the user's assets are absent.
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<dir> -DREPLAY=<script> -DOUT=<dir> [-DRECOMP=1] -P modern_game.cmake
#  1. --modern-check over the level-1 replay: every interpolable step pair re-renders exactly at t = 0 and t = 1,
#     and the captured pictures match the frames the video model rendered (palette fades excepted: the palette
#     is sampled at the swap).
#  2. With RECOMP: the draw-command capture (every blit with its registers and PC, every screen write, the object
#     lists) is identical under the translated executor and --interp-only.
if(NOT EXISTS "${ASSETS}/game_0400.bin" OR NOT EXISTS "${ASSETS}/disc2.bin")
  message("SKIPPED: no game data in ${ASSETS}")
  cmake_language(EXIT 77)
endif()
file(MAKE_DIRECTORY "${OUT}")

execute_process(
  COMMAND "${X2RECOMP}" --headless --assets "${ASSETS}" --no-reglog --report-every 0 --input-script "${REPLAY}"
          --frames 2000 --modern-check
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "x2recomp exited with ${rc}")
endif()
if(NOT out MATCHES "modern check: ([0-9]+) interpolable step pairs; t=0 exact ([0-9]+), t=1 exact ([0-9]+)")
  message(FATAL_ERROR "no modern check summary")
endif()
set(pairs ${CMAKE_MATCH_1})
if(pairs LESS 200 OR NOT CMAKE_MATCH_2 EQUAL pairs OR NOT CMAKE_MATCH_3 EQUAL pairs)
  message(FATAL_ERROR "interpolation endpoints not exact: ${CMAKE_MATCH_0}")
endif()
if(NOT out MATCHES "captured picture = rendered frame ([0-9]+) of ([0-9]+)")
  message(FATAL_ERROR "no picture comparison")
endif()
math(EXPR need "${CMAKE_MATCH_2} * 95 / 100")
if(CMAKE_MATCH_1 LESS need)
  message(FATAL_ERROR "captured pictures differ from rendered frames: ${CMAKE_MATCH_0}")
endif()

if(RECOMP)
  foreach(mode translated interp)
    set(extra)
    if(mode STREQUAL "interp")
      set(extra --interp-only)
    endif()
    execute_process(
      COMMAND "${X2RECOMP}" --headless --assets "${ASSETS}" --no-reglog --report-every 0 --input-script "${REPLAY}"
              --frames 1400 --draw-log "1100-1400:${OUT}/draw_${mode}.log" ${extra}
      RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "x2recomp (${mode}) exited with ${rc}: ${err}")
    endif()
  endforeach()
  file(SHA256 "${OUT}/draw_translated.log" h1)
  file(SHA256 "${OUT}/draw_interp.log" h2)
  file(SIZE "${OUT}/draw_interp.log" size)
  if(size LESS 100000 OR NOT h1 STREQUAL h2)
    message(FATAL_ERROR "draw capture differs between the translated executor and --interp-only (size ${size})")
  endif()
  message("draw capture identical under the translated executor and --interp-only (${size} bytes)")
endif()
