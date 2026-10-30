# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Replay regression test: runs one tests/replays/<name>.txt input script headless and compares the frame and
# cumulative audio hashes with tests/replays/<name>.hashes (hashes only, no game data). Exits 77 (CTest "Skipped")
# without assets.
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<dir> -DREPLAY=<name.txt> -DHASHES=<name.hashes> [-DEXTRA=<args>]
#              -P replay_check.cmake
# EXTRA (optional, ";"-separated) adds runtime options, so the same driver also pins an opt-in audio mode:
# --protect-music keeps the frame hashes and changes only the audio ones (docs/re-notes/music-dropouts.md §8).
# Re-record after an intended change with: python3 -I tools/trace/run_corpus.py --record-hashes --no-trace
if(NOT EXISTS "${ASSETS}/game_0400.bin" OR NOT EXISTS "${ASSETS}/disc2.bin")
  message("SKIPPED: no game data in ${ASSETS}; run tools/prep_assets.py with your own ADFs first")
  cmake_language(EXIT 77)
endif()
if(NOT EXISTS "${HASHES}")
  message(FATAL_ERROR "no hash manifest ${HASHES}; record it with tools/trace/run_corpus.py --record-hashes")
endif()

file(STRINGS "${HASHES}" want REGEX "^(frame|audio)hash [0-9]+ [0-9a-f]+$")
set(hash_args)
foreach(line IN LISTS want)
  if(line MATCHES "^framehash ([0-9]+) ")
    list(APPEND hash_args --hash-at ${CMAKE_MATCH_1})
  endif()
endforeach()
execute_process(
  COMMAND "${X2RECOMP}" --headless --report-every 0 --assets "${ASSETS}" --input-script "${REPLAY}" --audio-hash
          ${EXTRA} ${hash_args}
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "x2recomp exited with ${rc}\n${out}${err}")
endif()
if(NOT out MATCHES "restarts-via-\\$7032=0 ")
  message(FATAL_ERROR "the game restarted through $7032 during the replay\n${out}")
endif()
string(REGEX MATCHALL "(frame|audio)hash [0-9]+ [0-9a-f]+" got "${out}")
string(REPLACE ";" "\n" got_text "${got}")
string(REPLACE ";" "\n" want_text "${want}")
if(NOT got_text STREQUAL want_text)
  message(FATAL_ERROR "hashes differ from ${HASHES}\n--- expected\n${want_text}\n--- got\n${got_text}")
endif()
list(LENGTH got n)
message("all ${n} hashes match")
