# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# CTest driver for the game-data boot test. Exits 77 (CTest "Skipped") when the user's assets are absent.
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<dir> -DOUT=<dir> -P game_boot.cmake   (needs CMake >= 3.29)
if(NOT EXISTS "${ASSETS}/game_0400.bin" OR NOT EXISTS "${ASSETS}/disc2.bin")
  message("SKIPPED: no game data in ${ASSETS}; run tools/prep_assets.py with your own ADFs first")
  cmake_language(EXIT 77)
endif()

# Press fire every 50 frames from frame 400: title -> SELECT menu -> 1 player -> "GET READY" -> level 1.
set(fire_args)
foreach(f RANGE 400 1350 50)
  list(APPEND fire_args --fire ${f}:5)
endforeach()
execute_process(
  COMMAND "${X2RECOMP}" --headless --frames 1500 --dump-dir "${OUT}" --assets "${ASSETS}" --no-reglog ${fire_args}
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "x2recomp exited with ${rc}")
endif()

# Required milestones: VBL handler running, disc-id check passed, level 1 main loop running, bitplanes on,
# no exception restarts.
foreach(pattern
    "first VBL handler entry \\(\\$20C2\\) +[0-9]+"
    "disc-id check passed \\('X2B '\\) +[0-9]+"
    "main loop \\$78A4 first reached +[0-9]+ \\([1-9][0-9]* logic frames\\)"
    "bitplanes first displayed \\(DMA\\+BPU\\) +[0-9]+"
    "restarts-via-\\$7032=0 ")
  if(NOT out MATCHES "${pattern}")
    message(FATAL_ERROR "milestone not met: ${pattern}")
  endif()
endforeach()
