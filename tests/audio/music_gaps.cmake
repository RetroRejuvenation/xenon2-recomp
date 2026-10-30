# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# CTest driver for the music-gap test (docs/re-notes/music-dropouts.md). Exits 77 (CTest "Skipped") without the
# user's assets or without Python 3.
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<dir> -DOUT=<dir> -DREPLAY=<level1_play.txt> -DPYTHON=<python3>
#              -DCHECK=<check_music_gaps.py> -P music_gaps.cmake
# The one tune has long rests in which only AUD1 and AUD2 play, and those are exactly the two channels the
# sound effects take, so heavy firing makes the music inaudible there. Both the rests and the handover were
# confirmed frame for frame against the Amiberry reference captures, so this test pins them: the left pair
# (AUD0 + AUD3, never taken by an effect) must be silent over the tune's 14.8 s rest and playing before it.
if(NOT EXISTS "${ASSETS}/game_0400.bin" OR NOT EXISTS "${ASSETS}/disc2.bin")
  message("SKIPPED: no game data in ${ASSETS}; run tools/prep_assets.py with your own ADFs first")
  cmake_language(EXIT 77)
endif()
if(NOT PYTHON)
  message("SKIPPED: no Python 3")
  cmake_language(EXIT 77)
endif()

file(MAKE_DIRECTORY "${OUT}")
execute_process(
  COMMAND "${X2RECOMP}" --headless --report-every 0 --assets "${ASSETS}" --input-script "${REPLAY}"
    --audio-out "${OUT}/music_gaps.wav"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "x2recomp exited with ${rc}\n${out}${err}")
endif()

# level1_play: the in-game music starts at frame 974, plays until the tune's rest at 2840-3578, and the title
# music covers 10-450. Ranges are kept inside the measured intervals so a one-frame shift cannot fail the test.
execute_process(
  COMMAND "${PYTHON}" -I "${CHECK}" "${OUT}/music_gaps.wav"
    playing=20:440 playing=1000:2800 silent=2850:3570 playing=3600:4400
  RESULT_VARIABLE prc OUTPUT_VARIABLE pout ERROR_VARIABLE perr)
message("${pout}${perr}")
if(NOT prc EQUAL 0)
  message(FATAL_ERROR "music gaps differ from the reference-confirmed shape")
endif()

# The channel-allocation rule, with fire held from frame 1000 to 2000: a sound effect can only ever take AUD1
# or AUD2, and the fire effect (21 ticks, re-issued about every 24 frames) holds AUD2 for most of the time.
# The four watched words are the $B(a4) flags of the driver's shadow records.
execute_process(
  COMMAND "${X2RECOMP}" --headless --report-every 0 --frames 2100 --assets "${ASSETS}"
    --fire 460:5 --fire 650:5 --fire 700:5 --fire 750:5 --fire 800:5 --fire 850:5 --fire 900:5
    --joy 1000:F:1000 --poke 980-1990:0xCC6:0x27.w --poke 980-1990:0xCCA:0x3.w
    --watch-log "${OUT}/own.txt" --watch 1B76C,1B778,1B784,1B790
  RESULT_VARIABLE orc OUTPUT_VARIABLE oout ERROR_VARIABLE oerr)
if(NOT orc EQUAL 0)
  message(FATAL_ERROR "x2recomp exited with ${orc}\n${oout}${oerr}")
endif()
execute_process(
  COMMAND "${PYTHON}" -I "${CHECK}" --own "${OUT}/own.txt" 1050:1990
  RESULT_VARIABLE wrc OUTPUT_VARIABLE wout ERROR_VARIABLE werr)
message("${wout}${werr}")
if(NOT wrc EQUAL 0)
  message(FATAL_ERROR "the sound effects' channel allocation changed")
endif()
