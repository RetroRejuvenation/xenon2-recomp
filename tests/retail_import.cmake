# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# End-to-end check of the retail import with a real retail disc pair (IPF or flux; your own files): `x2recomp --import`
# must produce the retail image byte for byte (sha1 checked), disc 2 = the canonical disc 2, the key-track receipt and a
# manifest naming the variant; the imported assets must reproduce the recorded retail replay hashes; the opt-in protection
# patch must refuse without the confirmation (exit code 4), and with it produce the patched image (sha1 checked, no receipt,
# patched=true) that reproduces its own recorded hashes; --diagnose-image must report the key track. Exits 77 (Skipped)
# when the images are not there.
# Usage: cmake -DX2RECOMP=<exe> -DDISK1=<file> -DDISK2=<file> -DPRESSING=<id> -DGAME_SHA1=<sha1> -DPATCHED_SHA1=<sha1>
#              -DREPLAYS=<tests/replays> -DOUT=<scratch dir> -P retail_import.cmake
if(NOT EXISTS "${DISK1}" OR NOT EXISTS "${DISK2}")
  message("SKIPPED: the retail disc images ${DISK1} / ${DISK2} are not available")
  cmake_language(EXIT 77)
endif()
set(CANONICAL_DISC2_SHA1 "a748afc7a142335a40cc46fb7785dea820770241")
set(RECEIPT "x2recomp-copylock 2\nsignature rob-northen-comp\nsyncs ok\nseed 0x36FA43\nimage-sha1 ${GAME_SHA1}\n")
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

function(import_into dir)
  execute_process(COMMAND "${X2RECOMP}" --import "${DISK1}" "${DISK2}" --assets "${dir}" ${ARGN}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(IMPORT_RC "${rc}" PARENT_SCOPE)
  set(IMPORT_OUT "${out}${err}" PARENT_SCOPE)
endfunction()

function(replay_matches assets goldens name)
  execute_process(COMMAND "${CMAKE_COMMAND}" -DX2RECOMP=${X2RECOMP} -DASSETS=${assets}
      -DREPLAY=${REPLAYS}/${name}.txt -DHASHES=${goldens}/${name}.hashes -P "${CMAKE_CURRENT_LIST_DIR}/replay_check.cmake"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${name} on ${assets} does not match ${goldens}/${name}.hashes\n${out}${err}")
  endif()
endfunction()

# 1. the key-track import
import_into("${OUT}/keytrack")
if(NOT IMPORT_RC EQUAL 0)
  message(FATAL_ERROR "the retail import failed (rc=${IMPORT_RC})\n${IMPORT_OUT}")
endif()
file(SHA1 "${OUT}/keytrack/game_0400.bin" got)
if(NOT got STREQUAL "${GAME_SHA1}")
  message(FATAL_ERROR "the imported retail image has sha1 ${got}, expected ${GAME_SHA1}")
endif()
file(SHA1 "${OUT}/keytrack/disc2.bin" got)
if(NOT got STREQUAL "${CANONICAL_DISC2_SHA1}")
  message(FATAL_ERROR "disc 2 has sha1 ${got}, expected the canonical ${CANONICAL_DISC2_SHA1}")
endif()
file(READ "${OUT}/keytrack/copylock.txt" receipt)
if(NOT receipt STREQUAL RECEIPT)
  message(FATAL_ERROR "unexpected receipt:\n${receipt}")
endif()
file(READ "${OUT}/keytrack/manifest.txt" manifest)
foreach(want "variant=${PRESSING}\n" "pressing=${PRESSING}\n" "canonical=false\n" "patched=false\n" "copylock=present\n")
  string(FIND "${manifest}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "the manifest lacks ${want}\n${manifest}")
  endif()
endforeach()
replay_matches("${OUT}/keytrack" "${REPLAYS}/retail/${PRESSING_NUMBER}" level1_play)

# 2. diagnose-image reports the key track and the pressing, without printing game bytes
execute_process(COMMAND "${X2RECOMP}" --diagnose-image "${DISK1}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "Rob Northen Copylock verified, seed 0x36FA43" OR NOT out MATCHES "variant ${PRESSING}")
  message(FATAL_ERROR "--diagnose-image: rc=${rc}\n${out}${err}")
endif()

# 3. the opt-in patch: refused without the confirmation, nothing written
import_into("${OUT}/unconfirmed" --apply-protection-patch)
if(NOT IMPORT_RC EQUAL 4 OR EXISTS "${OUT}/unconfirmed/manifest.txt" OR NOT IMPORT_OUT MATCHES "YOUR retail disc 1")
  message(FATAL_ERROR "the patch without confirmation was not refused with exit code 4 and the disclaimer (rc=${IMPORT_RC})\n${IMPORT_OUT}")
endif()
# 4. confirmed: the patched image, no receipt, recorded as patched
import_into("${OUT}/patched" --apply-protection-patch --i-confirm-i-own-the-original)
if(NOT IMPORT_RC EQUAL 0)
  message(FATAL_ERROR "the confirmed patch import failed (rc=${IMPORT_RC})\n${IMPORT_OUT}")
endif()
file(SHA1 "${OUT}/patched/game_0400.bin" got)
if(NOT got STREQUAL "${PATCHED_SHA1}")
  message(FATAL_ERROR "the patched image has sha1 ${got}, expected ${PATCHED_SHA1}")
endif()
if(EXISTS "${OUT}/patched/copylock.txt")
  message(FATAL_ERROR "a patched import must not carry a receipt")
endif()
file(READ "${OUT}/patched/manifest.txt" manifest)
foreach(want "variant=retail-patched\n" "patched=true\n" "patched_ranges=$70F6+14,$76AE+4\n" "copylock=absent\n")
  string(FIND "${manifest}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "the patched manifest lacks ${want}\n${manifest}")
  endif()
endforeach()
replay_matches("${OUT}/patched" "${REPLAYS}/retail-patched/${PRESSING_NUMBER}" level1_play)
message("retail ${PRESSING}: import, receipt, replays, diagnose and the patch option all check out")
