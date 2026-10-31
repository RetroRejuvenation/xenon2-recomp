# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# CTest driver for the native importer on the user's own disk-image archives (Xenon2.zip and the TOSEC-style
# Xenon-2-Megablast_Amiga_EN.zip of nested zips). Exits 77 (CTest "Skipped") when none of them is present.
#   cmake -DX2RECOMP=<exe> -DZIPS=<a.zip;b.zip> -DREFERENCE=<generated/assets> -DOUT=<dir> -P game_import.cmake
#
# Checks: which images are accepted or rejected (and why), that the imported game image is byte-identical to
# the known-good one (and to REFERENCE/game_0400.bin from tools/prep_assets.py when present), and that the
# runtime boots to level 1 from the per-user data directory alone (no --assets, no generated/assets).
set(found)
foreach(z IN LISTS ZIPS)
  if(EXISTS "${z}")
    list(APPEND found "${z}")
  endif()
endforeach()
if(NOT found)
  message("SKIPPED: none of ${ZIPS} present (your own Xenon 2 disk images)")
  cmake_language(EXIT 77)
endif()

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
# Test-only identification of a correct import (never used to obtain or distribute game data).
set(GAME_SHA1 1d37b0142da49bd8a1c9bb03fe6a438ecad9fab4)
set(DISC2_SHA1 a748afc7a142335a40cc46fb7785dea820770241)
set(failures 0)
set(table "")

# import(<label> <expected rc> <expected text> <data dir or ""> paths...)
function(import label want_rc want_text data)
  set(dd "${OUT}/scratch")
  if(data)
    set(dd "${data}")
  endif()
  file(REMOVE_RECURSE "${dd}")
  execute_process(COMMAND "${X2RECOMP}" --import ${ARGN} --data-dir "${dd}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(FIND "${out}${err}" "${want_text}" pos)
  set(verdict "accepted")
  if(NOT rc EQUAL 0)
    set(verdict "rejected")
  endif()
  set(status "ok")
  if(NOT rc EQUAL want_rc OR pos EQUAL -1)
    set(status "FAIL (want rc ${want_rc} and '${want_text}')")
    math(EXPR f "${failures} + 1")
    set(failures ${f} PARENT_SCOPE)
    message("---- ${label}\n${out}${err}")
  endif()
  set(table "${table}  ${verdict} (rc ${rc}) ${label}: ${status}\n" PARENT_SCOPE)
  if(data AND rc EQUAL 0)
    foreach(f game_0400.bin disc2.bin)
      file(SHA1 "${dd}/assets/${f}" h)
      set(want ${GAME_SHA1})
      if(f STREQUAL "disc2.bin")
        set(want ${DISC2_SHA1})
      endif()
      if(NOT h STREQUAL want AND NOT (f STREQUAL "disc2.bin" AND label MATCHES "Deejay99"))
        message("---- ${label}: ${f} sha1 ${h}, expected ${want}")
        math(EXPR f2 "${failures} + 1")
        set(failures ${f2} PARENT_SCOPE)
      endif()
    endforeach()
    if(EXISTS "${REFERENCE}/game_0400.bin")
      execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${dd}/assets/game_0400.bin" "${REFERENCE}/game_0400.bin"
        RESULT_VARIABLE same)
      if(NOT same EQUAL 0)
        message("---- ${label}: game_0400.bin differs from ${REFERENCE}/game_0400.bin (tools/prep_assets.py)")
        math(EXPR f3 "${failures} + 1")
        set(failures ${f3} PARENT_SCOPE)
      endif()
    endif()
  endif()
endfunction()

set(boot_dir "")
foreach(z IN LISTS found)
  get_filename_component(zname "${z}" NAME)
  if(zname STREQUAL "Xenon2.zip")
    import("Xenon2.zip (both BS1 discs)" 0 "trainer loader and its patches are skipped" "${OUT}/xenon2" "${z}")
    set(boot_dir "${OUT}/xenon2")
    continue()
  endif()
  # The whole archive: the best disc 1/disc 2 pair is picked out of eleven images.
  import("${zname} (all 11 nested zips)" 0 "BS1 [t+3] trained release, disk 1 (sha1" "${OUT}/en" "${z}")
  if(NOT boot_dir)
    set(boot_dir "${OUT}/en")
  endif()
  # Each nested image on its own, and the supported pairs.
  set(src "${OUT}/src/${zname}")
  file(ARCHIVE_EXTRACT INPUT "${z}" DESTINATION "${src}")
  file(GLOB nested "${src}/*.zip")
  set(bs1_1 "")
  set(bs1_2 "")
  set(dj_1 "")
  set(dj_2 "")
  foreach(n IN LISTS nested)
    get_filename_component(nn "${n}" NAME)
    set(rc 2)
    set(text "")
    foreach(pair
        "[cr Black Monks]|2|Black Monks crack of the retail disk 1"
        "[cr PNA].zip|2|PNA crack of the retail disk 1"
        "[cr PNA][a]|2|PNA [a] crack of the retail disk 1"
        "[cr QTX]|2|Quartex (QTX) crack of the retail disk 1"
        "[cr VF].zip|0|Vision Factory (VF) one-disk crunch"
        "[cr VF][a]|0|Vision Factory (VF) [a] one-disk crunch"
        "[h Deejay99][t +3 BS1](Disk 1 of 2)|2|Deejay99 UAE-fix hack, disk 1"
        "[h Deejay99][t +3 BS1](Disk 2 of 2)|2|Deejay99 hack, disk 2"
        "[cr BS1][t +3 BS1](Disk 1 of 2).zip|2|BS1 [t+3] trained release, disk 1 (sha1"
        "[cr BS1][t +3 BS1](Disk 1 of 2)[a]|2|disk 1 [a] (alternate boot block)"
        "[cr BS1][t +3 BS1](Disk 2 of 2).zip|2|BS1 [t+3] trained release, disk 2")
      string(REPLACE "|" ";" p "${pair}")
      list(GET p 0 key)
      string(FIND "${nn}" "${key}" hit)
      if(NOT hit EQUAL -1)
        list(GET p 1 rc)
        list(GET p 2 text)
      endif()
    endforeach()
    if(NOT text)
      set(text "error:")
    endif()
    import("${nn} (alone)" ${rc} "${text}" "" "${n}")
    if(nn MATCHES "Deejay99.*Disk 1")
      set(dj_1 "${n}")
    elseif(nn MATCHES "Deejay99.*Disk 2")
      set(dj_2 "${n}")
    elseif(nn MATCHES "t \\+3 BS1\\]\\(Disk 1 of 2\\)\\.zip$")
      set(bs1_1 "${n}")
    elseif(nn MATCHES "t \\+3 BS1\\]\\(Disk 2 of 2\\)\\.zip$")
      set(bs1_2 "${n}")
    endif()
  endforeach()
  if(bs1_1 AND bs1_2)
    import("BS1 disk 1 + disk 2 as two paths" 0 "disc 2: " "${OUT}/pair" "${bs1_2}" "${bs1_1}")
  endif()
  if(dj_1 AND dj_2)
    import("Deejay99 disk 1 + disk 2 (accepted with a warning)" 0 "warning: disc 2 differs" "${OUT}/dj99" "${dj_1}" "${dj_2}")
  endif()
endforeach()
message("importer results:\n${table}")
if(failures GREATER 0)
  message(FATAL_ERROR "${failures} import check(s) failed")
endif()

# Boot from the per-user data directory only: run from an empty working directory, no --assets.
set(fire_args)
foreach(f RANGE 400 1350 50)
  list(APPEND fire_args --fire ${f}:5)
endforeach()
file(MAKE_DIRECTORY "${OUT}/run")
execute_process(
  COMMAND "${X2RECOMP}" --headless --frames 1500 --data-dir "${boot_dir}" --report-every 0 ${fire_args}
  WORKING_DIRECTORY "${OUT}/run"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "x2recomp (assets from ${boot_dir}) exited with ${rc}")
endif()
foreach(pattern
    "disc-id check passed \\('X2B '\\) +[0-9]+"
    "main loop \\$78A4 first reached +[0-9]+ \\([1-9][0-9]* logic frames\\)"
    "bitplanes first displayed \\(DMA\\+BPU\\) +[0-9]+"
    "restarts-via-\\$7032=0 ")
  if(NOT out MATCHES "${pattern}")
    message(FATAL_ERROR "milestone not met when booting from the per-user data directory: ${pattern}")
  endif()
endforeach()
