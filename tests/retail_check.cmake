# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# A retail image without its key-track receipt (copylock.txt) must be refused with exit code 3 and an explanation,
# not started (it would crash in the Copylock routine). Skipped (77) without retail assets.
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<retail assets dir> -DOUT=<scratch dir> -P retail_check.cmake
if(NOT EXISTS "${ASSETS}/game_0400.bin" OR NOT EXISTS "${ASSETS}/disc2.bin" OR NOT EXISTS "${ASSETS}/copylock.txt")
  message("SKIPPED: no retail assets in ${ASSETS}; build them with tools/retail_assets.py")
  cmake_language(EXIT 77)
endif()
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
file(COPY "${ASSETS}/game_0400.bin" "${ASSETS}/disc2.bin" DESTINATION "${OUT}")
execute_process(COMMAND "${X2RECOMP}" --headless --frames 2 --report-every 0 --assets "${OUT}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 3)
  message(FATAL_ERROR "expected exit code 3 for a retail image without copylock.txt, got ${rc}\n${out}${err}")
endif()
if(NOT err MATCHES "retail image" OR NOT err MATCHES "copylock.txt")
  message(FATAL_ERROR "the refusal does not explain itself:\n${err}")
endif()
# With the receipt next to it the same image starts.
file(COPY "${ASSETS}/copylock.txt" DESTINATION "${OUT}")
execute_process(COMMAND "${X2RECOMP}" --headless --frames 2 --report-every 0 --assets "${OUT}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "Copylock check at \\$70F6 is answered")
  message(FATAL_ERROR "the retail image did not start with its receipt (rc=${rc})\n${out}${err}")
endif()
# A damaged receipt is an error naming the file, not a silent start.
file(WRITE "${OUT}/copylock.txt" "x2recomp-copylock 1\nsignature nope\n")
execute_process(COMMAND "${X2RECOMP}" --headless --frames 2 --report-every 0 --assets "${OUT}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 3 OR NOT err MATCHES "copylock.txt")
  message(FATAL_ERROR "a damaged receipt was not refused (rc=${rc})\n${out}${err}")
endif()
