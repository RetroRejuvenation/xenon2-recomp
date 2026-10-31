# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Widescreen surround on the real game (Phase 9e). Exits 77 (CTest "Skipped") without the user's assets.
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<dir> -DREPLAY=<script> -P modern_wide.cmake
#  1. At 16:10, 16:9, 21:9 and 32:9 without interpolation, the centre 320 x 256 of every presented picture is the
#     rendered frame exactly ('centerhash' = 'framehash').
#  2. With interpolation (21:9), every interpolable step pair's centre is exact at t = 0 and t = 1.
# Golden hashes of surround pictures: tests/golden/modern_wide.txt (modern_midpoints.cmake).
if(NOT EXISTS "${ASSETS}/game_0400.bin" OR (NOT EXISTS "${ASSETS}/disc2.bin" AND NOT EXISTS "${ASSETS}/cd0"))
  message("SKIPPED: no game data in ${ASSETS}")
  cmake_language(EXIT 77)
endif()
# The CDTV release is driven with the same script: its assets (no disc2.bin, cd0/) and the frame window of its level 1 are
# passed in (FROM/TO/FRAMES/...: the defaults are the disk build's).
if(NOT DEFINED FRAMES)
  set(FRAMES 1460)
endif()
if(NOT DEFINED FROM)
  set(FROM 1150)
endif()
if(NOT DEFINED TO)
  set(TO 1450)
endif()
if(NOT DEFINED CHECK_FRAMES)
  set(CHECK_FRAMES 2000)
endif()
if(NOT DEFINED MIN_PAIRS)
  set(MIN_PAIRS 200)
endif()
set(base "${X2RECOMP}" --headless --assets "${ASSETS}" --no-reglog --report-every 0 --input-script "${REPLAY}")
foreach(ratio 16:10 16:9 21:9 32:9)
  execute_process(COMMAND ${base} --frames ${FRAMES} --modern wide=${ratio},scale=1,rate=50 --modern-out ${FROM}-${TO}:- --hash-every 1
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "x2recomp exited with ${rc}: ${err}")
  endif()
  string(REGEX MATCHALL "centerhash [1-9][0-9]* [0-9]+ [0-9a-f]+" centres "${out}")
  string(REGEX MATCHALL "presenthash [0-9]+ [0-9]+ [0-9a-f]+" presents "${out}")
  list(LENGTH centres n)
  if(n LESS 290)
    message(FATAL_ERROR "${ratio}: only ${n} surround pictures")
  endif()
  foreach(c IN LISTS centres)
    string(REGEX MATCH "centerhash [0-9]+ ([0-9]+) ([0-9a-f]+)" _ "${c}")
    if(NOT out MATCHES "framehash ${CMAKE_MATCH_1} ${CMAKE_MATCH_2}")
      message(FATAL_ERROR "${ratio}: centre of '${c}' differs from the rendered frame")
    endif()
  endforeach()
  # The surround itself is drawn: the whole pictures differ from their centres.
  list(GET presents 100 p)
  string(REGEX MATCH "presenthash [0-9]+ ([0-9]+) ([0-9a-f]+)" _ "${p}")
  if(out MATCHES "framehash ${CMAKE_MATCH_1} ${CMAKE_MATCH_2}")
    message(FATAL_ERROR "${ratio}: no surround in '${p}'")
  endif()
  message("${ratio}: ${n} pictures, centre identical to the rendered frame")
endforeach()
execute_process(COMMAND ${base} --frames ${CHECK_FRAMES} --modern interp,wide=21:9 --modern-check
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "surround ([0-9]+) pixels per side, centre exact at t=0 and t=1 in ([0-9]+) of ([0-9]+) pairs")
  message(FATAL_ERROR "no surround check (${rc}): ${out}${err}")
endif()
if(NOT CMAKE_MATCH_2 EQUAL CMAKE_MATCH_3 OR CMAKE_MATCH_3 LESS ${MIN_PAIRS})
  message(FATAL_ERROR "interpolated surround: ${CMAKE_MATCH_0}")
endif()
message("${CMAKE_MATCH_0}")
