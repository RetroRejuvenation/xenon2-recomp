# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# God mode, checked against the replay whose whole purpose is to die: tests/replays/game_over.txt pokes one
# ship, one credit and energy 1, takes hits, and reaches GAME OVER with ships $CCA = 0 and credits $CF0 = 0.
# Run with --god-mode, nothing may die: the ship and credit counts must still hold the values the replay poked
# and the energy must be full ($27). Both runs are made here, so the test still means something if the replay
# is ever changed. Exits 77 (CTest "Skipped") without game data.
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<dir> -DREPLAY=<game_over.txt> -P cheats_check.cmake
if(NOT EXISTS "${ASSETS}/game_0400.bin" OR NOT EXISTS "${ASSETS}/disc2.bin")
  message("SKIPPED: no game data in ${ASSETS}; run tools/prep_assets.py with your own ADFs first")
  cmake_language(EXIT 77)
endif()

# The last line of --watch-log is "<frame> <CCA> <CF0> <CC6> <idle> <blt>" in hex (docs/BUILDING.md).
function(x2_replay_end_state out_ships out_credits out_energy)
  cmake_parse_arguments(A "" "LOG" "EXTRA" ${ARGN})
  execute_process(
    COMMAND "${X2RECOMP}" --headless --report-every 0 --assets "${ASSETS}" --input-script "${REPLAY}"
            --watch-log "${A_LOG}" --watch CCA,CF0,CC6 ${A_EXTRA}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "x2recomp ${A_EXTRA} exited with ${rc}\n${out}${err}")
  endif()
  file(STRINGS "${A_LOG}" lines)
  list(POP_BACK lines last)
  if(NOT last MATCHES "^[0-9]+ ([0-9A-F]+) ([0-9A-F]+) ([0-9A-F]+) ")
    message(FATAL_ERROR "cannot read the last watch-log line: ${last}")
  endif()
  math(EXPR ships "0x${CMAKE_MATCH_1}")
  math(EXPR credits "0x${CMAKE_MATCH_2}")
  math(EXPR energy "0x${CMAKE_MATCH_3}")
  set(${out_ships} ${ships} PARENT_SCOPE)
  set(${out_credits} ${credits} PARENT_SCOPE)
  set(${out_energy} ${energy} PARENT_SCOPE)
endfunction()

x2_replay_end_state(base_ships base_credits base_energy LOG "${CMAKE_CURRENT_BINARY_DIR}/cheats_base.txt")
message("baseline:  ships=${base_ships} credits=${base_credits} energy=${base_energy}")
if(NOT base_ships EQUAL 0 OR NOT base_credits EQUAL 0)
  message(FATAL_ERROR "the baseline replay no longer reaches GAME OVER (ships=${base_ships} "
                      "credits=${base_credits}); this test can no longer tell whether god mode did anything")
endif()

x2_replay_end_state(god_ships god_credits god_energy
                    LOG "${CMAKE_CURRENT_BINARY_DIR}/cheats_god.txt" EXTRA --god-mode)
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
