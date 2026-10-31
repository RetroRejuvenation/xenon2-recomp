# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Upgraded graphics on the real game (Phase 9d, docs/ASSET-PACKS.md). Exits 77 (CTest "Skipped") without the
# user's assets. Everything it writes stays in OUT (the dump is the user's own game graphics: never commit it).
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<dir> -DREPLAY=<script> -DOUT=<dir> [-DRECOMP=1] -P modern_upgrade.cmake
#  1. --dump-assets writes the graphics of the level-1 replay and a manifest.
#  2. 'upgrade' without packs, and the dump loaded as a pack (every graphic replaced by its own picture), give
#     exactly the interpolated pictures of the plain replay.
#  3. Without interpolation, the dump as a pack gives exactly the rendered frames.
#  4. With RECOMP: the presented pictures (pack, surround, HD panel) are identical under the translated executor
#     and --interp-only.
if(NOT EXISTS "${ASSETS}/game_0400.bin" OR (NOT EXISTS "${ASSETS}/disc2.bin" AND NOT EXISTS "${ASSETS}/cd0"))
  message("SKIPPED: no game data in ${ASSETS}")
  cmake_language(EXIT 77)
endif()
# The CDTV release is driven with the same script: its assets (no disc2.bin, cd0/) and the frame window of its level 1 are
# passed in (FROM/TO/FRAMES/...: the defaults are the disk build's).
if(NOT DEFINED FRAMES)
  set(FRAMES 1460)
endif()
if(NOT DEFINED FROM)
  set(FROM 1150)
endif()
if(NOT DEFINED TO)
  set(TO 1450)
endif()
if(NOT DEFINED REC_FROM)
  set(REC_FROM 1100)
endif()
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(base "${X2RECOMP}" --headless --assets "${ASSETS}" --no-reglog --report-every 0 --input-script "${REPLAY}" --frames ${FRAMES})

function(run out_var)
  execute_process(COMMAND ${base} ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "x2recomp ${ARGN} exited with ${rc}: ${err}")
  endif()
  set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

# 1. The dump.
run(out --dump-assets "${OUT}/dump")
if(NOT out MATCHES "dump-assets: ([0-9]+) sprites, ([0-9]+) tiles, ([0-9]+) far lines" OR CMAKE_MATCH_1 LESS 20 OR
   CMAKE_MATCH_2 LESS 10 OR CMAKE_MATCH_3 LESS 100 OR NOT EXISTS "${OUT}/dump/manifest.json")
  message(FATAL_ERROR "dump incomplete: ${out}")
endif()
message("${CMAKE_MATCH_0}")

# 2. Interpolated pictures: plain, upgrade without packs, the dump as a pack.
foreach(mode plain upgrade pack)
  set(extra)
  if(mode STREQUAL "upgrade")
    set(extra --modern upgrade)
  elseif(mode STREQUAL "pack")
    set(extra --asset-pack "${OUT}/dump")
  endif()
  run(out --modern interp,scale=2 ${extra} --modern-out ${FROM}-${TO}:-)
  string(REGEX MATCHALL "presenthash [0-9]+ [0-9]+ [0-9a-f]+" ${mode} "${out}")
endforeach()
list(LENGTH plain n)
if(n LESS 300)
  message(FATAL_ERROR "only ${n} presented pictures")
endif()
if(NOT upgrade STREQUAL plain)
  message(FATAL_ERROR "'upgrade' without packs changed the pictures")
endif()
if(NOT pack STREQUAL plain)
  message(FATAL_ERROR "the dump loaded as a pack does not reproduce the original pictures")
endif()
message("upgrade without packs and the dump as a pack: ${n} interpolated pictures identical to the plain replay")

# 3. Without interpolation the pictures are the rendered frames (host frame 0 of --modern-out is emitted one frame
# early, before its frame has been run: skipped).
run(out --modern upgrade,scale=1,rate=50 --asset-pack "${OUT}/dump" --modern-out ${FROM}-${TO}:- --hash-every 1)
string(REGEX MATCHALL "presenthash [1-9][0-9]* [0-9]+ [0-9a-f]+" presents "${out}")
list(LENGTH presents n)
if(n LESS 290)
  message(FATAL_ERROR "only ${n} presented pictures")
endif()
foreach(p IN LISTS presents)
  string(REGEX MATCH "presenthash [0-9]+ ([0-9]+) ([0-9a-f]+)" _ "${p}")
  if(NOT out MATCHES "framehash ${CMAKE_MATCH_1} ${CMAKE_MATCH_2}")
    message(FATAL_ERROR "replacement pictures without interpolation: '${p}' differs from the rendered frame")
  endif()
endforeach()
message("the dump as a pack without interpolation: ${n} pictures identical to the rendered frames")

# 4. Translated executor and --interp-only: identical presented pictures.
if(RECOMP)
  foreach(mode translated interp)
    set(extra)
    if(mode STREQUAL "interp")
      set(extra --interp-only)
    endif()
    run(out --modern interp,scale=2,wide=21:9,hdpanel --asset-pack "${OUT}/dump" --modern-out ${REC_FROM}-${TO}:- ${extra})
    string(REGEX MATCHALL "(presenthash|centerhash) [0-9]+ [0-9]+ [0-9a-f]+" ${mode} "${out}")
  endforeach()
  list(LENGTH translated n)
  if(n LESS 600 OR NOT translated STREQUAL interp)
    message(FATAL_ERROR "modern pictures differ between the translated executor and --interp-only (${n} hashes)")
  endif()
  message("modern pictures identical under the translated executor and --interp-only (${n} hashes)")
endif()
