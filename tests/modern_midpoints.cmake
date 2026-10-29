# Golden hashes of interpolated pictures (renderer-version specific, kept apart from the faithful goldens): for
# each "replay frame" line of tests/golden/modern_midpoints.txt, the first interpolable step pair swapped in at or
# after the frame, rendered at scale 3 with the default options at t = 0, .25, .5, .75 and 1 (--modern-shots).
# Exits 77 (CTest "Skipped") without the user's assets.
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<dir> -DREPLAYS=<dir> -DOUT=<dir> -DGOLDEN=<file> [-DUPDATE=1]
#        -P modern_midpoints.cmake      (UPDATE=1 rewrites the hashes after checking the PNGs in OUT by eye)
if(NOT EXISTS "${ASSETS}/game_0400.bin" OR NOT EXISTS "${ASSETS}/disc2.bin")
  message("SKIPPED: no game data in ${ASSETS}")
  cmake_language(EXIT 77)
endif()
file(STRINGS "${GOLDEN}" lines REGEX "^[a-z0-9_]+ [0-9]+")
set(got_text "")
foreach(line IN LISTS lines)
  string(REGEX MATCH "^([a-z0-9_]+) ([0-9]+)" _ "${line}")
  set(replay ${CMAKE_MATCH_1})
  set(frame ${CMAKE_MATCH_2})
  math(EXPR frames "${frame} + 40")
  file(MAKE_DIRECTORY "${OUT}/${replay}_${frame}")
  execute_process(
    COMMAND "${X2RECOMP}" --headless --assets "${ASSETS}" --no-reglog --report-every 0
            --input-script "${REPLAYS}/${replay}.txt" --frames ${frames} --modern interp,scale=3
            --modern-shots "${frame}:${OUT}/${replay}_${frame}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "x2recomp exited with ${rc}: ${err}")
  endif()
  string(REGEX MATCHALL "modernshot t[0-9]+ [0-9a-f]+" shots "${out}")
  string(REPLACE "modernshot " "" shots "${shots}")
  string(REPLACE ";" " " shots "${shots}")
  string(APPEND got_text "${replay} ${frame} ${shots}\n")
endforeach()
file(WRITE "${OUT}/modern_midpoints.txt" "${got_text}")
if(UPDATE)
  file(READ "${GOLDEN}" old)
  string(REGEX MATCH "^(#[^\n]*\n)*" header "${old}")
  file(WRITE "${GOLDEN}" "${header}${got_text}")
  message("golden file updated: ${GOLDEN}")
  return()
endif()
set(want_text "")
foreach(line IN LISTS lines)
  string(APPEND want_text "${line}\n")
endforeach()
if(NOT got_text STREQUAL want_text)
  message(FATAL_ERROR "interpolated pictures differ from ${GOLDEN}\n--- expected\n${want_text}--- got\n${got_text}"
                      "Look at the PNGs in ${OUT}; if the change is intended, re-run with -DUPDATE=1.")
endif()
list(LENGTH lines n)
message("all ${n} interpolated step pairs match")
