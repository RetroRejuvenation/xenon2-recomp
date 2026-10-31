# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# The CDTV release's CD audio on real data (Skipped, exit 77, without the imported game assets or the disc): one replay is
# played with the CD mixed in (--music cdtv) and checked against recorded goldens:
#   - the frame hashes are the committed ones (tests/replays/cdtv/NAME.hashes): the CD never changes the game;
#   - the audio hashes with the CD mixed in are tests/replays/cdtv/NAME.cd.hashes (the player runs in emulated time, so
#     the mix is exact and repeatable);
#   - the game's CD requests (frame, line, event, track, caller) are tests/replays/cdtv/NAME.cues;
#   - a second run with no CD tracks available starts, says the music is silent, and gives exactly the committed
#     (CD-free) frame and audio hashes.
# Re-record the .cd.hashes and .cues goldens after an intended change with -DRECORD=1.
#
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<cdtv assets> -DZIP=<CDTV disc image> -DCDTV_DIR=<CD audio import dir>
#              -DREPLAYS=<tests/replays/cdtv> -DNAME=<replay> -DOUT=<scratch dir> [-DRECORD=1] -P cdtv_release_audio.cmake
if(NOT EXISTS "${ASSETS}/game_0400.bin" OR NOT EXISTS "${ASSETS}/manifest.txt")
  message("SKIPPED: no CDTV release assets in ${ASSETS} (import your CDTV disc: x2recomp --import PATH)")
  cmake_language(EXIT 77)
endif()
file(STRINGS "${ASSETS}/manifest.txt" build_line REGEX "^build=cdtv")
if(NOT build_line)
  message("SKIPPED: ${ASSETS} is not a CDTV release import")
  cmake_language(EXIT 77)
endif()
if(NOT EXISTS "${CDTV_DIR}/manifest.txt")
  if(NOT EXISTS "${ZIP}")
    message("SKIPPED: no CD audio in ${CDTV_DIR} and no CDTV disc image at ${ZIP}")
    cmake_language(EXIT 77)
  endif()
  execute_process(COMMAND "${X2RECOMP}" --import-cdtv "${ZIP}" --cdtv "${CDTV_DIR}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "importing the CD audio failed (${rc})\n${out}${err}")
  endif()
endif()
file(MAKE_DIRECTORY "${OUT}")

set(base_hashes "${REPLAYS}/${NAME}.hashes")
file(STRINGS "${base_hashes}" base REGEX "^(frame|audio)hash [0-9]+ [0-9a-f]+$")
set(hash_args)
foreach(line IN LISTS base)
  if(line MATCHES "^framehash ([0-9]+) ")
    list(APPEND hash_args --hash-at ${CMAKE_MATCH_1})
  endif()
endforeach()

function(play outvar errvar cdtv_path logfile)
  execute_process(
    COMMAND "${X2RECOMP}" --game cdtv --headless --report-every 0 --assets "${ASSETS}" --input-script "${REPLAYS}/${NAME}.txt"
            --audio-hash --music cdtv --cdtv "${cdtv_path}" --cdtv-log "${logfile}" ${hash_args}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "x2recomp exited with ${rc}\n${out}${err}")
  endif()
  string(REGEX MATCHALL "(frame|audio)hash [0-9]+ [0-9a-f]+" got "${out}")
  set(${outvar} "${got}" PARENT_SCOPE)
  set(${errvar} "${err}" PARENT_SCOPE)
endfunction()

function(lines_of kind list_in out)
  set(r)
  foreach(l IN LISTS list_in)
    if(l MATCHES "^${kind}hash ")
      list(APPEND r "${l}")
    endif()
  endforeach()
  set(${out} "${r}" PARENT_SCOPE)
endfunction()

# The cue log without its header line, one event per line.
function(cues_of logfile out)
  file(STRINGS "${logfile}" all)
  set(r)
  foreach(l IN LISTS all)
    if(NOT l MATCHES "^#" AND NOT l MATCHES " (open|read) ")
      list(APPEND r "${l}")
    endif()
  endforeach()
  string(REPLACE ";" "\n" t "${r}")
  set(${out} "${t}\n" PARENT_SCOPE)
endfunction()

# --- with the CD ---
play(got1 err1 "${CDTV_DIR}" "${OUT}/cues.log")
lines_of(frame "${got1}" frames1)
lines_of(audio "${got1}" audio1)
lines_of(frame "${base}" frames_want)
lines_of(audio "${base}" audio_nocd)
if(NOT "${frames1}" STREQUAL "${frames_want}")
  string(REPLACE ";" "\n" a "${frames_want}")
  string(REPLACE ";" "\n" b "${frames1}")
  message(FATAL_ERROR "the CD changed the game: frame hashes differ from ${base_hashes}\n--- expected\n${a}\n--- got\n${b}")
endif()
if("${audio1}" STREQUAL "${audio_nocd}")
  message(FATAL_ERROR "the audio hashes are the CD-free ones: the CD was not mixed in\n${err1}")
endif()
string(REPLACE ";" "\n" audio_text "${audio1}")
cues_of("${OUT}/cues.log" cues_text)

set(cd_hashes "${REPLAYS}/${NAME}.cd.hashes")
set(cues_golden "${REPLAYS}/${NAME}.cues")
if(RECORD)
  file(WRITE "${cd_hashes}" "${audio_text}\n")
  file(WRITE "${cues_golden}" "# CD requests of ${NAME} (frame line event track caller); tests/cdtv/cdtv_release_audio.cmake\n${cues_text}")
  message("recorded ${cd_hashes} and ${cues_golden}")
else()
  file(STRINGS "${cd_hashes}" want_audio REGEX "^audiohash ")
  string(REPLACE ";" "\n" want_text "${want_audio}")
  if(NOT audio_text STREQUAL want_text)
    message(FATAL_ERROR "the audio with the CD differs from ${cd_hashes}\n--- expected\n${want_text}\n--- got\n${audio_text}")
  endif()
  file(READ "${cues_golden}" want_cues)
  string(REGEX REPLACE "^#[^\n]*\n" "" want_cues "${want_cues}")
  if(NOT cues_text STREQUAL want_cues)
    message(FATAL_ERROR "the CD requests differ from ${cues_golden}\n--- expected\n${want_cues}\n--- got\n${cues_text}")
  endif()
endif()

# --- the same replay, no CD tracks to play from ---
play(got2 err2 "${OUT}/no_such_disc" "${OUT}/cues2.log")
if(NOT err2 MATCHES "the music will be silent")
  message(FATAL_ERROR "no CD audio, but no 'silent' message\n${err2}")
endif()
string(REGEX MATCHALL "silent" silent_lines "${err2}")
list(LENGTH silent_lines n_silent)
if(NOT n_silent EQUAL 1)
  message(FATAL_ERROR "the silent message should come once, came ${n_silent} times\n${err2}")
endif()
string(REPLACE ";" "\n" a "${base}")
string(REPLACE ";" "\n" b "${got2}")
if(NOT a STREQUAL b)
  message(FATAL_ERROR "without CD tracks the hashes are not the committed CD-free ones\n--- expected\n${a}\n--- got\n${b}")
endif()
message("CD audio: frames unchanged, ${NAME}.cd.hashes and ${NAME}.cues match; without CD tracks the music is silent (once) and the hashes are the CD-free ones")
