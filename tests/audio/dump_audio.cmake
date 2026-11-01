# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# --dump-audio on a replay (docs/re-notes/asset-packs-sfx.md): the run must be the replay's own (its frame and audio hashes
# unchanged: a dump observes, never changes the machine), the dump must list exactly the sounds recorded in KEYS (hashes of the
# samples, no audio), and every WAV must be the sample its key names (check_dump.py reads them without the runtime). Exits 77
# (CTest "Skipped") without game data.
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<dir> -DREPLAY=<name.txt> -DHASHES=<name.hashes> -DKEYS=<keys file> -DOUT=<dir>
#              -DPYTHON=<python3 or empty> -DCHECK=<check_dump.py> -P dump_audio.cmake
# Re-record KEYS after an intended change with: python3 -I tests/audio/check_dump.py <dump dir> --list > <keys file>
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

file(REMOVE_RECURSE "${OUT}")
file(STRINGS "${HASHES}" want REGEX "^(frame|audio)hash [0-9]+ [0-9a-f]+$")
set(hash_args)
foreach(line IN LISTS want)
  if(line MATCHES "^framehash ([0-9]+) ")
    list(APPEND hash_args --hash-at ${CMAKE_MATCH_1})
  endif()
endforeach()
execute_process(
  COMMAND "${X2RECOMP}" --headless --report-every 0 --assets "${ASSETS}" --input-script "${REPLAY}" --audio-hash
          --dump-audio "${OUT}" ${hash_args}
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "x2recomp exited with ${rc}\n${out}${err}")
endif()
string(REGEX MATCHALL "(frame|audio)hash [0-9]+ [0-9a-f]+" got "${out}")
string(REPLACE ";" "\n" got_text "${got}")
string(REPLACE ";" "\n" want_text "${want}")
if(NOT got_text STREQUAL want_text)
  message(FATAL_ERROR "the run differs from ${HASHES} with --dump-audio\n--- expected\n${want_text}\n--- got\n${got_text}")
endif()

if(NOT PYTHON)
  message("SKIPPED: no Python 3 to read the dump")
  cmake_language(EXIT 77)
endif()
execute_process(COMMAND "${PYTHON}" -I "${CHECK}" "${OUT}" --list
  RESULT_VARIABLE rc OUTPUT_VARIABLE listing ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the dump is not consistent:\n${err}")
endif()
file(READ "${KEYS}" expected)
string(REGEX REPLACE "[\r\n]+$" "" expected "${expected}")
string(REGEX REPLACE "[\r\n]+$" "" listing "${listing}")
string(REGEX REPLACE "\n[0-9]+ entries ok$" "" listing "${listing}")
if(NOT listing STREQUAL expected)
  message(FATAL_ERROR "the dump lists other sounds than ${KEYS}\n--- expected\n${expected}\n--- got\n${listing}")
endif()
message("dump matches ${KEYS}")
