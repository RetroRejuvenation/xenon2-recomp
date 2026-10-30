# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# God mode, checked against replays that kill the ship. Exits 77 (CTest "Skipped") without game data.
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<dir> -DMODE=<energy|crush|equivalence> -P cheats_check.cmake
#
#   energy       tests/replays/game_over.txt exists to die: it pokes one ship, one credit and energy 1, takes
#                hits, and reaches GAME OVER with ships $CCA = 0 and credits $CF0 = 0. With --god-mode
#                nothing may die, so both counts must still hold what the replay poked and energy must be
#                full. Both runs are made, and the test fails loudly if the baseline ever stops dying,
#                because then it would prove nothing.
#   crush        tests/replays/boss1.txt reaches the one death that costs no energy: the push-back counter
#                $42E walks from -1 to -$11 and $5FE0 calls the death routine with the energy bar still full
#                (frame 2381 as recorded). The baseline must execute that call; with --god-mode the hook must
#                report skipping a death whose caller is $5FE4, and the game must carry on to the same level
#                as the baseline, which is what shows the push-back sequence was ended rather than left on
#                its last step for ever.
#   equivalence  recompiled builds only: the same god-mode run must give identical frame hashes through the
#                translated executor and through --interp-only, because the hook moves the PC and must behave
#                the same in both.
if(NOT EXISTS "${ASSETS}/game_0400.bin" OR NOT EXISTS "${ASSETS}/disc2.bin")
  message("SKIPPED: no game data in ${ASSETS}; run tools/prep_assets.py with your own ADFs first")
  cmake_language(EXIT 77)
endif()

function(x2_run)
  cmake_parse_arguments(A "" "OUT_VAR" "EXTRA" ${ARGN})
  execute_process(
    COMMAND "${X2RECOMP}" --headless --report-every 0 --assets "${ASSETS}" --input-script "${REPLAY}" ${A_EXTRA}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "x2recomp ${A_EXTRA} exited with ${rc}\n${out}${err}")
  endif()
  set(${A_OUT_VAR} "${out}${err}" PARENT_SCOPE)
endfunction()

# The last line of --watch-log is "<frame> <hex words...> <idle> <blt>" (docs/BUILDING.md).
function(x2_end_state log out_a out_b out_c)
  file(STRINGS "${log}" lines)
  list(POP_BACK lines last)
  if(NOT last MATCHES "^[0-9]+ ([0-9A-F]+) ([0-9A-F]+) ([0-9A-F]+) ")
    message(FATAL_ERROR "cannot read the last watch-log line: ${last}")
  endif()
  math(EXPR a "0x${CMAKE_MATCH_1}")
  math(EXPR b "0x${CMAKE_MATCH_2}")
  math(EXPR c "0x${CMAKE_MATCH_3}")
  set(${out_a} ${a} PARENT_SCOPE)
  set(${out_b} ${b} PARENT_SCOPE)
  set(${out_c} ${c} PARENT_SCOPE)
endfunction()

if(MODE STREQUAL "energy")
  set(base_log "${CMAKE_CURRENT_BINARY_DIR}/cheats_energy_base.txt")
  set(god_log "${CMAKE_CURRENT_BINARY_DIR}/cheats_energy_god.txt")
  x2_run(OUT_VAR out EXTRA --watch-log "${base_log}" --watch CCA,CF0,CC6)
  x2_end_state("${base_log}" base_ships base_credits base_energy)
  message("baseline:  ships=${base_ships} credits=${base_credits} energy=${base_energy}")
  if(NOT base_ships EQUAL 0 OR NOT base_credits EQUAL 0)
    message(FATAL_ERROR "the baseline replay no longer reaches GAME OVER (ships=${base_ships} "
                        "credits=${base_credits}); this test can no longer tell whether god mode did anything")
  endif()

  x2_run(OUT_VAR out EXTRA --god-mode --watch-log "${god_log}" --watch CCA,CF0,CC6)
  x2_end_state("${god_log}" god_ships god_credits god_energy)
  message("god mode:  ships=${god_ships} credits=${god_credits} energy=${god_energy}")
  if(god_ships EQUAL 0)
    message(FATAL_ERROR "god mode: the ship still died (ships=0)")
  endif()
  if(god_credits EQUAL 0)
    message(FATAL_ERROR "god mode: a credit was still spent (credits=0)")
  endif()
  if(NOT god_energy EQUAL 39) # $27
    message(FATAL_ERROR "god mode: energy is ${god_energy}, expected 39 ($27, full)")
  endif()
  message("god mode kept the ship alive through the replay that is built to kill it")

elseif(MODE STREQUAL "crush")
  # The baseline must still reach the energy-independent death, or there is nothing to cover.
  set(pc_log "${CMAKE_CURRENT_BINARY_DIR}/cheats_crush_pc.txt")
  set(base_log "${CMAKE_CURRENT_BINARY_DIR}/cheats_crush_base.txt")
  set(god_log "${CMAKE_CURRENT_BINARY_DIR}/cheats_crush_god.txt")
  x2_run(OUT_VAR out EXTRA --pc-log "0-6000:5FE0:${pc_log}" --watch-log "${base_log}" --watch E04,CD0,CC6)
  file(STRINGS "${pc_log}" pc_lines REGEX "005FE0")
  list(LENGTH pc_lines n_crush)
  x2_end_state("${base_log}" base_level base_scroll base_energy)
  message("baseline:  $5FE0 (push-back death) executed ${n_crush} time(s), ends on level ${base_level}")
  if(n_crush EQUAL 0)
    message(FATAL_ERROR "the baseline replay never reaches the push-back death at $5FE0; this test can no "
                        "longer tell whether god mode covers it")
  endif()

  x2_run(OUT_VAR god_out EXTRA --god-mode --input-debug --watch-log "${god_log}" --watch E04,CD0,CC6)
  string(REGEX MATCHALL "\\[god\\] frame [0-9]+: death skipped, called from \\$005FE4" skips "${god_out}")
  list(LENGTH skips n_skips)
  x2_end_state("${god_log}" god_level god_scroll god_energy)
  message("god mode:  ${n_skips} push-back death(s) skipped, ends on level ${god_level}")
  if(n_skips EQUAL 0)
    message(FATAL_ERROR "god mode did not skip the push-back death at $5FE0\n${god_out}")
  endif()
  # Clearing $42E must end the sequence rather than leave it on its last step: if it had not, the ship would
  # be stuck there and the game could not go on reaching the same level as the baseline.
  if(NOT god_level EQUAL base_level)
    message(FATAL_ERROR "god mode: the game ends on level ${god_level}, the baseline on ${base_level}; "
                        "the push-back sequence looks stuck")
  endif()
  message("god mode survived the death that costs no energy, and the game carried on")

elseif(MODE STREQUAL "equivalence")
  # The hook moves the PC, which ends a translated run (runtime/recomp/x2r_runtime.h X2R_BOUNDARY). The
  # executor must then continue exactly as the interpreter does.
  set(hash_args --hash-at 2400 --hash-at 2600 --hash-at 3000 --hash-at 4000 --hash-at 5000 --hash-at 5999)
  x2_run(OUT_VAR trans EXTRA --god-mode ${hash_args})
  x2_run(OUT_VAR interp EXTRA --god-mode --interp-only ${hash_args})
  string(REGEX MATCHALL "framehash [0-9]+ [0-9a-f]+" trans_h "${trans}")
  string(REGEX MATCHALL "framehash [0-9]+ [0-9a-f]+" interp_h "${interp}")
  list(LENGTH trans_h n)
  if(n EQUAL 0)
    message(FATAL_ERROR "no frame hashes in the translated run\n${trans}")
  endif()
  if(NOT trans_h STREQUAL interp_h)
    string(REPLACE ";" "\n" t "${trans_h}")
    string(REPLACE ";" "\n" i "${interp_h}")
    message(FATAL_ERROR "god mode differs between the translated executor and the interpreter\n"
                        "--- translated\n${t}\n--- --interp-only\n${i}")
  endif()
  message("all ${n} god-mode frame hashes identical, translated and interpreted")

else()
  message(FATAL_ERROR "MODE must be energy, crush or equivalence (got '${MODE}')")
endif()
