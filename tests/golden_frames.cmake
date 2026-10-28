# Golden-frame test: runs the scripted game session headless and compares FNV-1a hashes of the rendered RGBA
# frames with tests/golden/game_frames.txt (hashes only, no images). Exits 77 (CTest "Skipped") without assets.
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<dir> -DOUT=<dir> -DGOLDEN=<file> [-DUPDATE=1] -P golden_frames.cmake
# UPDATE=1 rewrites the golden file from this run (after checking the frames by eye).
if(NOT EXISTS "${ASSETS}/game_0400.bin" OR NOT EXISTS "${ASSETS}/disc2.bin")
  message("SKIPPED: no game data in ${ASSETS}; run tools/prep_assets.py with your own ADFs first")
  cmake_language(EXIT 77)
endif()

# Same input script as game_boot: fire every 50 frames from 400 (title -> menu -> GET READY -> level 1).
set(fire_args)
foreach(f RANGE 400 1350 50)
  list(APPEND fire_args --fire ${f}:5)
endforeach()
# Frames: title, title later, menu, GET READY, level 1 start, a few hundred frames into level 1.
set(frames 150 300 470 700 860 900 1000 1100 1250 1400)
set(hash_args)
foreach(f IN LISTS frames)
  list(APPEND hash_args --hash-at ${f})
endforeach()
file(MAKE_DIRECTORY "${OUT}")
execute_process(
  COMMAND "${X2RECOMP}" --headless --frames 1450 --assets "${ASSETS}" --report-every 0 ${fire_args} ${hash_args}
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "x2recomp exited with ${rc}\n${out}${err}")
endif()
string(REGEX MATCHALL "framehash [0-9]+ [0-9a-f]+" got "${out}")
string(REPLACE ";" "\n" got_text "${got}")
file(WRITE "${OUT}/game_frames.txt" "${got_text}\n")
if(UPDATE)
  file(WRITE "${GOLDEN}" "${got_text}\n")
  message("golden file updated: ${GOLDEN}")
  return()
endif()
file(READ "${GOLDEN}" want_text)
string(STRIP "${want_text}" want_text)
if(NOT got_text STREQUAL want_text)
  message(FATAL_ERROR "frame hashes differ from ${GOLDEN}\n--- expected\n${want_text}\n--- got\n${got_text}\n"
                      "Inspect with --screenshot-at F:file.png; if the change is intended, re-run with -DUPDATE=1.")
endif()
list(LENGTH got n)
message("all ${n} golden frames match")
