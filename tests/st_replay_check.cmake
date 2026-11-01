# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Atari ST replay regression test: runs tests/replays/st/<NAME>.txt headless on the user's own Kixx dump (sha1 below) and compares the
# frame hashes and the cumulative audio hashes with tests/replays/st/<NAME>.hashes (hashes only, no game data). Exits 77 (CTest
# "Skipped") without the dump. Also pins what the run report says about the end state (no unmapped accesses, the level reached, ...).
# Usage: cmake -DX2RECOMP=<exe> -DDISK_DIR=<dir with the .st> -DREPLAYS=<tests/replays/st> -DNAME=<name> [-DEXPECT=key=value;...]
#              -P st_replay_check.cmake
# EXPECT keys are fields of the last periodic report line (level, energy, ships, credits, score, unmapped).
# Re-record after an intended change with: python3 tools/trace/make_st_replays.py --record --exe build/x2recomp --disk <kixx.st>
set(KIXX_SHA1 "323d6859635b6b82c86363db048b26e0f3df994c")
set(disk "")
file(GLOB candidates "${DISK_DIR}/*.st")
foreach(f IN LISTS candidates)
  file(SHA1 "${f}" h)
  if(h STREQUAL KIXX_SHA1)
    set(disk "${f}")
  endif()
endforeach()
if(NOT disk)
  message("SKIPPED: the Kixx dump (sha1 ${KIXX_SHA1}) is not in '${DISK_DIR}'")
  cmake_language(EXIT 77)
endif()
set(script "${REPLAYS}/${NAME}.txt")
set(hashes "${REPLAYS}/${NAME}.hashes")
if(NOT EXISTS "${script}" OR NOT EXISTS "${hashes}")
  message(FATAL_ERROR "missing ${script} or ${hashes}; record them with tools/trace/make_st_replays.py")
endif()

file(STRINGS "${hashes}" want REGEX "^(frame|audio)hash [0-9]+ [0-9a-f]+$")
set(hash_args)
foreach(line IN LISTS want)
  if(line MATCHES "^framehash ([0-9]+) ")
    list(APPEND hash_args --hash-at ${CMAKE_MATCH_1})
  endif()
endforeach()
execute_process(
  COMMAND "${X2RECOMP}" --game st --headless --st-disk "${disk}" --st-fdc-speed 8 --report-every 500 --audio-hash
          --input-script "${script}" ${hash_args}
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "x2recomp exited with ${rc}\n${out}${err}")
endif()
if(NOT out MATCHES "[ ,]0 unmapped accesses")
  message(FATAL_ERROR "unmapped accesses during the replay\n${out}")
endif()
string(REGEX MATCHALL "(frame|audio)hash [0-9]+ [0-9a-f]+" got "${out}")
string(REPLACE ";" "\n" got_text "${got}")
string(REPLACE ";" "\n" want_text "${want}")
if(NOT got_text STREQUAL want_text)
  message(FATAL_ERROR "hashes differ from ${hashes}\n--- expected\n${want_text}\n--- got\n${got_text}")
endif()

if(EXPECT)
  string(REGEX MATCHALL "frame [0-9]+ pc=[^\n]*" lines "${out}")
  list(POP_BACK lines last)
  foreach(item IN LISTS EXPECT)
    if(NOT item MATCHES "^([a-z]+)=([0-9]+)$")
      message(FATAL_ERROR "bad EXPECT item '${item}'")
    endif()
    set(key ${CMAKE_MATCH_1})
    set(value ${CMAKE_MATCH_2})
    if(NOT last MATCHES "[ ]${key}=([0-9]+)")
      message(FATAL_ERROR "no ${key}= in the last report line: ${last}")
    endif()
    if(NOT CMAKE_MATCH_1 STREQUAL value)
      message(FATAL_ERROR "${NAME}: ${key} is ${CMAKE_MATCH_1} at the end, expected ${value} (${last})")
    endif()
  endforeach()
endif()
list(LENGTH got n)
message("all ${n} hashes match")
