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
# A receipt written for another image arms nothing: refused with that reason, also when it is an old receipt that names none.
file(READ "${ASSETS}/copylock.txt" GOOD_RECEIPT)
string(REGEX REPLACE "image-sha1 [0-9a-f]+" "image-sha1 0000000000000000000000000000000000000000" WRONG_RECEIPT "${GOOD_RECEIPT}")
file(WRITE "${OUT}/copylock.txt" "${WRONG_RECEIPT}")
execute_process(COMMAND "${X2RECOMP}" --headless --frames 2 --report-every 0 --assets "${OUT}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 3 OR NOT err MATCHES "different game image")
  message(FATAL_ERROR "a receipt for another image was not refused (rc=${rc})\n${out}${err}")
endif()
file(WRITE "${OUT}/copylock.txt" "x2recomp-copylock 1\nsignature rob-northen-comp\nsyncs ok\nseed 0x36FA43\n")
execute_process(COMMAND "${X2RECOMP}" --headless --frames 2 --report-every 0 --assets "${OUT}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 3 OR NOT err MATCHES "old receipt")
  message(FATAL_ERROR "an old receipt was not refused (rc=${rc})\n${out}${err}")
endif()
# A damaged receipt is an error naming the file, not a silent start.
file(WRITE "${OUT}/copylock.txt" "x2recomp-copylock 1\nsignature nope\n")
execute_process(COMMAND "${X2RECOMP}" --headless --frames 2 --report-every 0 --assets "${OUT}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 3 OR NOT err MATCHES "copylock.txt")
  message(FATAL_ERROR "a damaged receipt was not refused (rc=${rc})\n${out}${err}")
endif()
