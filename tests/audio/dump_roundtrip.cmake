# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# A --dump-audio dump loaded back as a pack must sound like the original (docs/re-notes/asset-packs-sfx.md section 4): the replay is
# run plain (--audio-out), with --dump-audio, and with the dump as --asset-pack as a directory and as an uncompressed ZIP; the two
# pack runs must be identical and correlate with the plain one above MIN (the output filter is off for the comparison: pack voices
# are the author's final sound and bypass it), and `--check-asset-pack --against` must see the dump's sounds. Exits 77 (CTest
# "Skipped") without game data or Python.
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<dir> -DREPLAY=<name.txt> -DOUT=<dir> -DPYTHON=<python3> -DCHECK=<check_roundtrip.py>
#              -DMIN=0.99 -P dump_roundtrip.cmake
set(need_disc2 TRUE)
if(EXISTS "${ASSETS}/manifest.txt")
  file(STRINGS "${ASSETS}/manifest.txt" build_line REGEX "^build=cdtv")
  if(build_line)
    set(need_disc2 FALSE)
  endif()
endif()
if(NOT EXISTS "${ASSETS}/game_0400.bin" OR (need_disc2 AND NOT EXISTS "${ASSETS}/disc2.bin"))
  message("SKIPPED: no game data in ${ASSETS}; run tools/prep_assets.py with your own ADFs first")
  cmake_language(EXIT 77)
endif()
if(NOT PYTHON)
  message("SKIPPED: no Python 3")
  cmake_language(EXIT 77)
endif()

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(common --headless --report-every 0 --assets "${ASSETS}" --input-script "${REPLAY}" --audio-filter none)
function(run_x2 name)
  execute_process(COMMAND "${X2RECOMP}" ${common} ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${name}: x2recomp exited with ${rc}\n${out}${err}")
  endif()
  set(last_out "${out}${err}" PARENT_SCOPE)
endfunction()

run_x2(plain --audio-out "${OUT}/plain.wav")
run_x2(dump --dump-audio "${OUT}/dump")
execute_process(COMMAND "${PYTHON}" -I "${CHECK}" zip "${OUT}/dump" "${OUT}/dump.zip" RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "cannot zip the dump\n${err}")
endif()
foreach(kind dir zip)
  if(kind STREQUAL "dir")
    set(pack "${OUT}/dump")
  else()
    set(pack "${OUT}/dump.zip")
  endif()
  run_x2(pack_${kind} --asset-pack "${pack}" --audio-out "${OUT}/${kind}.wav")
  if(NOT last_out MATCHES "[1-9][0-9]* sound clips")
    message(FATAL_ERROR "the ${kind} pack registered no sound clips\n${last_out}")
  endif()
  execute_process(COMMAND "${PYTHON}" -I "${CHECK}" corr "${OUT}/plain.wav" "${OUT}/${kind}.wav" ${MIN}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  message("${kind}: ${out}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "the ${kind} pack does not sound like the original (want correlation >= ${MIN})\n${out}${err}")
  endif()
  # coverage over the dump: every sound of the dump is in the pack
  execute_process(COMMAND "${X2RECOMP}" --check-asset-pack "${pack}" --against "${OUT}/dump"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0 OR NOT "${out}${err}" MATCHES "sound effects ([0-9]+)/([0-9]+)")
    message(FATAL_ERROR "--check-asset-pack --against failed on the ${kind} pack (${rc})\n${out}${err}")
  endif()
  if(NOT CMAKE_MATCH_1 EQUAL CMAKE_MATCH_2 OR CMAKE_MATCH_1 EQUAL 0)
    message(FATAL_ERROR "coverage of the dump is ${CMAKE_MATCH_1}/${CMAKE_MATCH_2}\n${out}${err}")
  endif()
endforeach()
execute_process(COMMAND "${PYTHON}" -I "${CHECK}" same "${OUT}/dir.wav" "${OUT}/zip.wav" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the directory pack and the ZIP pack render different audio")
endif()
message("the dump loaded back (directory and ZIP) reproduces the original audio")
