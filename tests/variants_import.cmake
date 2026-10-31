# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# End-to-end check of the variant registry with real disk images (your own files, any folder; the default is the
# git-ignored generated/sources-extra, laid out as docs/re-notes/sources.md describes): every image is imported through
# `x2recomp --import` and must give the documented outcome - the canonical image (sha1 checked) for the BS1 family, the
# Timecircle trainer set and the "Xenon 2 & 4 Others" compilation, the recorded plain image of each cracked retail disk 1
# with its own replay hashes, the exit code 3 refusals (damaged disc 2, other programs, cracks that are not recoverable) and, for a WHDLoad install of the first retail pressing, the refusal without the key track and the
# explicit patch route, which must give the same patched image as the IPF route. Exits 77 (Skipped) when the folder or the
# canonical disc 2 is missing.
# Usage: cmake -DX2RECOMP=<exe> -DSRC=<folder with tosec2010-adf/ and whd/> -DDISC2=<canonical disk 2 ADF>
#              -DREPLAYS=<tests/replays> -DOUT=<scratch dir> -P variants_import.cmake
set(T "${SRC}/tosec2010-adf")
set(W "${SRC}/whd")
if(NOT IS_DIRECTORY "${T}" OR NOT EXISTS "${DISC2}")
  message("SKIPPED: no variant images in ${SRC} (tosec2010-adf/) or no canonical disk 2 at ${DISC2}")
  cmake_language(EXIT 77)
endif()
set(CANONICAL_GAME_SHA1 "1d37b0142da49bd8a1c9bb03fe6a438ecad9fab4")
set(CANONICAL_DISC2_SHA1 "a748afc7a142335a40cc46fb7785dea820770241")
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(n_checked 0)

function(import_case name expect_rc)
  set(inputs ${ARGN})
  file(MAKE_DIRECTORY "${OUT}/${name}")
  execute_process(COMMAND "${X2RECOMP}" --import ${inputs} --assets "${OUT}/${name}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(CASE_RC "${rc}" PARENT_SCOPE)
  set(CASE_OUT "${out}${err}" PARENT_SCOPE)
  if(NOT rc EQUAL ${expect_rc})
    message(FATAL_ERROR "${name}: expected exit code ${expect_rc}, got ${rc}\n${out}${err}")
  endif()
endfunction()

function(expect_text name text)
  string(FIND "${CASE_OUT}" "${text}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "${name}: the output lacks \"${text}\"\n${CASE_OUT}")
  endif()
endfunction()

function(expect_manifest name)
  file(READ "${OUT}/${name}/manifest.txt" manifest)
  foreach(want ${ARGN})
    string(FIND "${manifest}" "${want}\n" pos)
    if(pos EQUAL -1)
      message(FATAL_ERROR "${name}: the manifest lacks ${want}\n${manifest}")
    endif()
  endforeach()
endfunction()

function(expect_sha1 name file want)
  file(SHA1 "${OUT}/${name}/${file}" got)
  if(NOT got STREQUAL "${want}")
    message(FATAL_ERROR "${name}: ${file} has sha1 ${got}, expected ${want}")
  endif()
endfunction()

function(replay_matches name goldens replay)
  execute_process(COMMAND "${CMAKE_COMMAND}" -DX2RECOMP=${X2RECOMP} -DASSETS=${OUT}/${name}
      -DREPLAY=${REPLAYS}/${replay}.txt -DHASHES=${goldens}/${replay}.hashes -P "${CMAKE_CURRENT_LIST_DIR}/replay_check.cmake"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${name}: ${replay} does not match ${goldens}/${replay}.hashes\n${out}${err}")
  endif()
endfunction()

function(have file)
  if(NOT EXISTS "${file}")
    message("SKIPPED: ${file} is not available")
    cmake_language(EXIT 77)
  endif()
endfunction()

set(D1 "Xenon 2 - Megablast (1989)(Image Works)(Disk 1 of 2)")
set(O4 "Xenon 2 - Megablast & 4 Others (19xx)(-)")

# --- the BS1 family and the compilation: canonical image -------------------------------------------------------------
have("${T}/${D1}[cr BS1][h Timecircle][t +3 BS1].adf")
import_case(timecircle 0 "${T}/${D1}[cr BS1][h Timecircle][t +3 BS1].adf" "${DISC2}")
expect_sha1(timecircle game_0400.bin "${CANONICAL_GAME_SHA1}")
expect_manifest(timecircle "variant=bs1-timecircle" "canonical=true" "patched=false" "copylock=absent")
if(NOT CASE_OUT MATCHES "disc 1: " OR CASE_OUT MATCHES "not a recognised image")
  message(FATAL_ERROR "timecircle: the disk was not recognised\n${CASE_OUT}")
endif()

have("${T}/${O4}(Disk 1 of 2).adf")
import_case(others4 0 "${T}/${O4}(Disk 1 of 2).adf" "${T}/${O4}(Disk 2 of 2).adf")
expect_sha1(others4 game_0400.bin "${CANONICAL_GAME_SHA1}")
expect_manifest(others4 "variant=others4" "canonical=true")
expect_text(others4 "equivalent for the game")
import_case(others4_a 3 "${T}/${O4}(Disk 1 of 2)[a].adf" "${T}/${O4}(Disk 2 of 2)[a].adf")
expect_text(others4_a "a damaged copy")
# the [a] disk 1 is fine with the good disk 2
import_case(others4_a_d1 0 "${T}/${O4}(Disk 1 of 2)[a].adf" "${T}/${O4}(Disk 2 of 2).adf")
expect_sha1(others4_a_d1 game_0400.bin "${CANONICAL_GAME_SHA1}")
expect_manifest(others4_a_d1 "variant=others4-a")

# --- other programs and unrecoverable cracks: refused --------------------------------------------------------------------
import_case(demo 3 "${T}/Xenon 2 - Megablast (demo-playable) (1989)(Image Works).adf" "${DISC2}")
expect_text(demo "playable demo")
import_case(lgcss 3 "${T}/Rick Dangerous & Xenon II - Megablast & 3D Pool & Tech (1989)(L.G.C.S.S.).adf" "${DISC2}")
expect_text(lgcss "not the game")
import_case(predators 3 "${T}/${D1}[cr Predators][t +9 Predators].adf" "${DISC2}")
expect_text(predators "entry code")
import_case(flashtro_a 3 "${T}/${D1}[cr Flashtro][t +16 Flashtro][a].adf" "${DISC2}")
expect_text(flashtro_a "not been analysed")

# --- cracked retail disk 1: the plain image, with its own replay hashes -----------------------------------------------
# id | disk | sha1 of the game image (the bytes at $70C00, as tools/variants/crack_retaildisk.py recovers them)
foreach(case
    "crack-pna|[cr PNA]|1599c37d894f4e276bee64d6f3bf874cc80e1f01"
    "crack-qtx|[cr QTX]|86094773cb2b31c8b9886e242026ba7ba9bba721"
    "crack-flashtro-16|[cr Flashtro][t +16 Flashtro]|70fcb3d8c7eaf937adeafa7db13dab9887d2b6ce")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 id)
  list(GET parts 1 tag)
  list(GET parts 2 want)
  have("${T}/${D1}${tag}.adf")
  import_case(${id} 0 "${T}/${D1}${tag}.adf" "${DISC2}")
  expect_sha1(${id} game_0400.bin "${want}")
  expect_sha1(${id} disc2.bin "${CANONICAL_DISC2_SHA1}")
  expect_manifest(${id} "variant=${id}" "canonical=false" "patched=false" "copylock=absent")
  expect_text(${id} "not the canonical")
  foreach(replay level1_play ending_natural)
    replay_matches(${id} "${REPLAYS}/cracks/${id}" ${replay})
  endforeach()
endforeach()

# --- a WHDLoad install of the first retail pressing: no key track, so refused; the patch route equals the IPF route ---
set(WHD0297 "${W}/Xenon2_v1.9_2Disk_0297.zip")
if(EXISTS "${WHD0297}")
  import_case(whd0297 3 "${WHD0297}")
  expect_text(whd0297 "retail original, first pressing (SPS 0297)")
  expect_text(whd0297 "cannot carry the Copylock key track")
  expect_text(whd0297 "--apply-protection-patch")
  import_case(whd0297_unconfirmed 4 "${WHD0297}" --apply-protection-patch)
  import_case(whd0297_patched 0 "${WHD0297}" --apply-protection-patch --i-confirm-i-own-the-original)
  expect_sha1(whd0297_patched game_0400.bin "1ca85d734896572f954c156085846032c3efb8b7") # the same as the IPF route (retail_import_0297)
  expect_sha1(whd0297_patched disc2.bin "${CANONICAL_DISC2_SHA1}")
  expect_manifest(whd0297_patched "variant=retail-patched" "patched=true" "copylock=absent" "canonical=false")
  if(EXISTS "${OUT}/whd0297_patched/copylock.txt")
    message(FATAL_ERROR "a patched import must not carry a receipt")
  endif()
else()
  message("note: ${WHD0297} is not available; the WHDLoad checks were not run")
endif()
message("variant registry: every case checked out")
