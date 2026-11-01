# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# The Atari ST release's hooks, options and coverage pokes on the user's own Kixx dump (sha1 323d6859...f3df994c). Exits 77
# (CTest "Skipped") without it. Pins state variables and counts from the run report, never pictures or game bytes.
# Usage: cmake -DX2RECOMP=<exe> -DDISK_DIR=<dir with the .st> -DREPLAYS=<tests/replays/st> -DSCRATCH=<dir>
#              -DMODE=<god|credits|levels|ending|scores> -P st_hooks_check.cmake
#
#   god      hold_fire.txt: unassisted (its poke makes the ship fragile once), the ship is killed and the game offers a continue (credits 3 -> 2);
#            with --god-mode the poked ship survives (no continue) and credits and energy stay full.
#   credits  --credits 6: a new game starts with 6 and the continue takes one (5); the baseline continues from 3 to 2.
#   levels   level2.txt: $E04=1 and $A07=$FF load level 2 through the game's own path, which plays on with nothing unmapped
#            (the partial-DMA-at-the-top-of-RAM regression).
#   ending   ending_l5.txt: level 5's end event, the guardians, the core and the game's own ending, then level 1 again with the
#            ending's extra credit.
#   scores   hold_fire.txt with --persist-dir: the name entry saves the table; a second run loads it into the game's RAM table
#            ($8FAA, first score long = the first run's best); --god-mode in the second run does not overwrite the file.
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
file(MAKE_DIRECTORY "${SCRATCH}")

function(st_run script out_var)
  cmake_parse_arguments(A "" "" "EXTRA" ${ARGN})
  execute_process(
    COMMAND "${X2RECOMP}" --game st --headless --st-disk "${disk}" --st-fdc-speed 8 --report-every 500
            --input-script "${REPLAYS}/${script}" ${A_EXTRA}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "x2recomp ${script} ${A_EXTRA} exited with ${rc}\n${out}${err}")
  endif()
  set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

# The last "frame N ..." report line's fields, as variables with the given prefix.
function(st_state out prefix)
  string(REGEX MATCHALL "frame [0-9]+ [^\n]*" lines "${out}")
  list(POP_BACK lines last)
  foreach(key level energy ships credits score unmapped)
    if(last MATCHES "${key}=([0-9]+)")
      set(${prefix}_${key} ${CMAKE_MATCH_1} PARENT_SCOPE)
    else()
      message(FATAL_ERROR "no ${key}= in: ${last}")
    endif()
  endforeach()
endfunction()

function(expect what got want)
  if(NOT "${got}" STREQUAL "${want}")
    message(FATAL_ERROR "${what}: got ${got}, expected ${want}")
  endif()
endfunction()

if(MODE STREQUAL "god")
  st_run(hold_fire.txt base)
  st_state("${base}" b)
  if(b_credits GREATER_EQUAL 3 AND b_ships GREATER_EQUAL 3)
    message(FATAL_ERROR "the unassisted baseline no longer loses a ship or a credit (ships ${b_ships}, credits ${b_credits}); it proves nothing")
  endif()
  st_run(hold_fire.txt god EXTRA --god-mode)
  st_state("${god}" g)
  # The poke leaves one ship and energy 1; god mode refills the energy first, so nothing dies: no continue, no game over. (The hook for the
  # crush death that costs no energy is covered by the synthetic st_game test.)
  expect("god mode ships (the poked one)" ${g_ships} 1)
  expect("god mode credits" ${g_credits} 3)
  expect("god mode energy" ${g_energy} 39)
  expect("god mode unmapped" ${g_unmapped} 0)
  if(NOT god MATCHES "god mode: on")
    message(FATAL_ERROR "no god-mode notice in the output")
  endif()
elseif(MODE STREQUAL "credits")
  st_run(hold_fire.txt base)
  st_state("${base}" b)
  expect("baseline credits after one continue" ${b_credits} 2)
  st_run(hold_fire.txt six EXTRA --credits 6)
  st_state("${six}" s)
  expect("credits after one continue from 6" ${s_credits} 5)
  if(NOT six MATCHES "credits: a new game will start with 6 \\(CHEAT\\)")
    message(FATAL_ERROR "no credits notice in the output")
  endif()
elseif(MODE STREQUAL "levels")
  st_run(level2.txt out)
  st_state("${out}" s)
  expect("level" ${s_level} 2)
  expect("unmapped accesses" ${s_unmapped} 0)
  if(NOT s_score GREATER 2150)
    message(FATAL_ERROR "level 2 is not being played (score ${s_score})")
  endif()
  if(NOT out MATCHES "coverage pokes active")
    message(FATAL_ERROR "no coverage-poke notice in the output")
  endif()
elseif(MODE STREQUAL "ending")
  st_run(ending_l5.txt out)
  st_state("${out}" s)
  expect("level after the ending" ${s_level} 1)
  expect("credits after the ending (+1)" ${s_credits} 4)
  expect("unmapped accesses" ${s_unmapped} 0)
elseif(MODE STREQUAL "scores")
  set(dir "${SCRATCH}/scores")
  file(REMOVE_RECURSE "${dir}")
  st_run(hold_fire.txt first EXTRA --persist-dir "${dir}")
  if(NOT first MATCHES "no saved table" OR NOT first MATCHES "high scores: saved")
    message(FATAL_ERROR "the first run did not start empty and save the table:\n${first}")
  endif()
  if(NOT EXISTS "${dir}/highscores-st.bin")
    message(FATAL_ERROR "no highscores-st.bin")
  endif()
  file(SIZE "${dir}/highscores-st.bin" size)
  expect("score file size" ${size} 214)
  file(SHA1 "${dir}/highscores-st.bin" before)
  # The game's table in RAM after the second run's first frames: the loaded table's top score is the first run's best (2500 = $09C4: the score when the poked ship was killed).
  st_run(hold_fire.txt second EXTRA --persist-dir "${dir}" --frames 3000 --st-ram-dump-at 3000:${SCRATCH}/scores_ram.bin)
  if(NOT second MATCHES "high scores: loaded")
    message(FATAL_ERROR "the second run did not load the table:\n${second}")
  endif()
  file(READ "${SCRATCH}/scores_ram.bin" top HEX OFFSET 36778 LIMIT 4) # $8FAA
  expect("restored top score long" ${top} "000009c4")
  # A cheating run neither loads over nor overwrites the file.
  st_run(hold_fire.txt cheat EXTRA --persist-dir "${dir}" --god-mode --credits 5)
  file(SHA1 "${dir}/highscores-st.bin" after)
  expect("score file after a cheating run" ${after} ${before})
else()
  message(FATAL_ERROR "unknown MODE '${MODE}'")
endif()
message("st hooks (${MODE}): ok")
