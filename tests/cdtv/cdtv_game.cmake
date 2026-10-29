# CDTV music layer on real data: imports the user's own CDTV disc image (exit 77 = CTest "Skipped" without it),
# checks the track list and the programme-start rule, then, with game data, plays replays with --music cdtv and
# checks that
#   - the CD cues (which track at which frame) match tests/cdtv/golden/*.cues;
#   - the game is unaffected: frame hashes equal the committed replay hashes, the RNG and frame-counter words
#     ($1F50, $1FEE-$1FF4, $404, $41C, $E04, $CDC) are identical every frame to a run without CD music, and so
#     is the whole chip RAM at the end of the replay;
#   - the mixed audio is exact and repeatable (cumulative audio hashes and a WAV render hash in
#     tests/cdtv/golden/hashes.txt), and identical under --interp-only in a recompiled build;
#   - without CD audio, --music cdtv warns once and the audio equals the Amiga music bit for bit;
#   - the game's pause loop issues pause/resume cues.
# Re-record the goldens after an intended change with -DRECORD=1.
#
# Usage: cmake -DX2RECOMP=<exe> -DZIP=<CDTV zip> -DASSETS=<dir> -DREPLAYS=<dir> -DGOLDEN=<dir> -DOUT=<dir>
#              -DCDTV_DIR=<import dir> -DRECOMP=<0|1> -P cdtv_game.cmake
if(NOT EXISTS "${ZIP}")
  message("SKIPPED: no CDTV disc image at ${ZIP} (your own Xenon 2 CDTV disc; see docs/BUILDING.md)")
  cmake_language(EXIT 77)
endif()
file(MAKE_DIRECTORY "${OUT}")

# --- import (through --import, which recognises a CD image) ---
execute_process(COMMAND "${X2RECOMP}" --import "${ZIP}" --cdtv "${CDTV_DIR}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "import failed (${rc})\n${out}${err}")
endif()
# Programme lengths measured on the Redump image; start sector 151 for tracks 2-11, 150 for 12-13.
set(expect
  "2 151 12141612" "3 151 18712512" "4 151 16382268" "5 151 13993812" "6 151 13626312" "7 151 18036312"
  "8 151 11862312" "9 151 14817012" "10 151 15654912" "11 151 17749956" "12 150 8834700" "13 150 15155700")
foreach(e IN LISTS expect)
  string(REPLACE " " ";" f "${e}")
  list(GET f 0 n)
  list(GET f 1 s)
  list(GET f 2 frames)
  if(NOT out MATCHES "track ${n}: programme from sector ${s} \\(CUE INDEX 01 at 13[78]\\), ${frames} frames")
    message(FATAL_ERROR "track ${n}: expected start ${s} and ${frames} frames\n${out}")
  endif()
endforeach()
if(NOT out MATCHES "imported 12 CD audio tracks")
  message(FATAL_ERROR "import incomplete\n${out}")
endif()
message("import: 12 tracks, programme-start rule and lengths as expected")

if(NOT EXISTS "${ASSETS}/game_0400.bin" OR NOT EXISTS "${ASSETS}/disc2.bin")
  message("no game data in ${ASSETS}: replay checks skipped (import checks passed)")
  return()
endif()

set(goldens "")
function(check_golden name text)
  if(RECORD)
    file(WRITE "${GOLDEN}/${name}" "${text}")
    message("recorded ${GOLDEN}/${name}")
    return()
  endif()
  file(READ "${GOLDEN}/${name}" want)
  if(NOT want STREQUAL text)
    message(FATAL_ERROR "${name} differs from ${GOLDEN}/${name}\n--- expected\n${want}\n--- got\n${text}")
  endif()
endfunction()

function(run_x2 outvar)
  execute_process(COMMAND "${X2RECOMP}" --headless --report-every 0 --assets "${ASSETS}" ${ARGN}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "x2recomp ${ARGN} exited with ${rc}\n${out}${err}")
  endif()
  set(${outvar} "${out}${err}" PARENT_SCOPE)
endfunction()

# --- boss1: title, level 1, mid-level shop, level 1 boss, shop, level 2 ---
set(replay "${REPLAYS}/boss1.txt")
file(STRINGS "${REPLAYS}/boss1.hashes" committed REGEX "^framehash ")
set(hash_args)
foreach(line IN LISTS committed)
  string(REGEX REPLACE "^framehash ([0-9]+) .*" "\\1" f "${line}")
  list(APPEND hash_args --hash-at ${f})
endforeach()
set(watch --watch 1F50,1FEE,1FF0,1FF2,1FF4,404,41C,E04,CDC)
run_x2(cd_out --input-script "${replay}" --music cdtv --cdtv "${CDTV_DIR}" --cdtv-log "${OUT}/boss1.cues" --audio-hash
  ${hash_args} --watch-log "${OUT}/boss1_cd.watch" ${watch} --dump-ram-at 5999:${OUT}/boss1_cd.ram)
run_x2(plain_out --input-script "${replay}" --watch-log "${OUT}/boss1_plain.watch" ${watch}
  --dump-ram-at 5999:${OUT}/boss1_plain.ram)
string(REGEX MATCHALL "framehash [0-9]+ [0-9a-f]+" got "${cd_out}")
if(NOT got STREQUAL committed)
  message(FATAL_ERROR "frame hashes with --music cdtv differ from boss1.hashes:\n${got}")
endif()
foreach(kind watch ram)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}/boss1_cd.${kind}" "${OUT}/boss1_plain.${kind}" RESULT_VARIABLE diff)
  if(NOT diff EQUAL 0)
    message(FATAL_ERROR "game state differs with --music cdtv (${OUT}/boss1_cd.${kind} vs boss1_plain.${kind})")
  endif()
endforeach()
file(READ "${OUT}/boss1.cues" cues)
check_golden(boss1.cues "${cues}")
string(REGEX MATCHALL "audiohash [0-9]+ [0-9a-f]+" audio "${cd_out}")
string(REPLACE ";" "\n" audio "${audio}")
message("boss1: frame hashes = committed, state words identical every frame, chip RAM identical, cues as golden")

# --- recompiled build: the translated executor and the interpreter give the same cues and audio ---
if(RECOMP)
  run_x2(interp_out --interp-only --input-script "${replay}" --music cdtv --cdtv "${CDTV_DIR}"
    --cdtv-log "${OUT}/boss1_interp.cues" --audio-hash ${hash_args})
  string(REGEX MATCHALL "audiohash [0-9]+ [0-9a-f]+" interp_audio "${interp_out}")
  string(REPLACE ";" "\n" interp_audio "${interp_audio}")
  file(READ "${OUT}/boss1_interp.cues" interp_cues)
  if(NOT interp_audio STREQUAL audio OR NOT interp_cues STREQUAL cues)
    message(FATAL_ERROR "--interp-only differs from the translated executor with --music cdtv")
  endif()
  message("recomp: --interp-only cues and audio identical")
endif()

# --- every level's track: levels 3-5 through the replays' level-change path ---
foreach(r level3 level4 level5)
  run_x2(o --input-script "${REPLAYS}/${r}.txt" --frames 1400 --music cdtv --cdtv "${CDTV_DIR}" --cdtv-log "${OUT}/${r}.cues")
  file(READ "${OUT}/${r}.cues" c)
  check_golden(${r}.cues "${c}")
endforeach()

# --- the game's pause (P, then any key) ---
run_x2(o --input-script "${REPLAYS}/level1_play.txt" --frames 1450 --key 1300:19 --fire 1400:5 --music cdtv
  --cdtv "${CDTV_DIR}" --cd-pause stop --cdtv-log "${OUT}/pause.cues")
file(READ "${OUT}/pause.cues" c)
if(NOT c MATCHES "\n1[0-9]+ [0-9]+ pause 0 [0-9A-F]+\n1[0-9]+ [0-9]+ resume 0")
  message(FATAL_ERROR "no pause/resume cues\n${c}")
endif()

# --- exact, repeatable renders: cumulative audio hashes of boss1, and a WAV of the title in mix mode ---
foreach(i 1 2)
  run_x2(o --frames 400 --music mix --cdtv "${CDTV_DIR}" --audio-out "${OUT}/title_mix_${i}.wav")
  file(SHA256 "${OUT}/title_mix_${i}.wav" wav_${i})
endforeach()
if(NOT wav_1 STREQUAL wav_2)
  message(FATAL_ERROR "two renders of the same run differ")
endif()
check_golden(hashes.txt "${audio}\nwav title_mix_400 ${wav_1}\n")

# --- fallback: no CD audio -> one warning, the Amiga music bit for bit ---
file(STRINGS "${REPLAYS}/boss1.hashes" committed_audio REGEX "^audiohash ")
set(audio_args)
foreach(line IN LISTS committed_audio)
  string(REGEX REPLACE "^audiohash ([0-9]+) .*" "\\1" f "${line}")
  list(APPEND audio_args --hash-at ${f})
endforeach()
run_x2(fb --input-script "${replay}" --music cdtv --cdtv "${OUT}/no-such-dir" --audio-hash ${audio_args})
string(REGEX MATCHALL "using the Amiga music" warn "${fb}")
list(LENGTH warn nwarn)
string(REGEX MATCHALL "audiohash [0-9]+ [0-9a-f]+" fb_audio "${fb}")
if(NOT nwarn EQUAL 1 OR NOT fb_audio STREQUAL committed_audio)
  message(FATAL_ERROR "fallback: ${nwarn} warnings, audio:\n${fb_audio}\nexpected:\n${committed_audio}")
endif()
message("all CDTV checks passed")
