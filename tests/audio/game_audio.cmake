# CTest driver for the game audio test. Exits 77 (CTest "Skipped") when the user's assets are absent.
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<dir> -DOUT=<dir> -DPYTHON=<python3 or empty> -DCHECK=<check_wav.py>
#              -P game_audio.cmake   (needs CMake >= 3.29)
# Runs title -> SELECT -> GET READY -> level 1 with fire held for 100 frames in level 1, writes the Paula
# output and the music/sound driver call log, and checks: title music starts within the first frames, the
# in-game music is started after the level load, firing queues sound effects, every channel is used, nothing
# clips, and (with Python) the title and level music segments are audible on both sides.
if(NOT EXISTS "${ASSETS}/game_0400.bin" OR NOT EXISTS "${ASSETS}/disc2.bin")
  message("SKIPPED: no game data in ${ASSETS}; run tools/prep_assets.py with your own ADFs first")
  cmake_language(EXIT 77)
endif()

file(MAKE_DIRECTORY "${OUT}")
set(fire_frame 1450)
execute_process(
  COMMAND "${X2RECOMP}" --headless --frames 1700 --dump-dir "${OUT}" --assets "${ASSETS}" --no-reglog
    --fire 460:5 --fire 950:5 --fire 1200:5 --fire ${fire_frame}:100
    --audio-out "${OUT}/game_audio.wav" --audio-cues "${OUT}/audio_cues.txt"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "x2recomp exited with ${rc}")
endif()

if(NOT out MATCHES "clipped samples 0 ")
  message(FATAL_ERROR "audio clipped")
endif()
foreach(ch 0 1 2 3)
  if(NOT out MATCHES "AUD${ch}: dma starts [1-9][0-9]*,[^\n]*audible ([0-9]+)\\.")
    message(FATAL_ERROR "AUD${ch} never started")
  endif()
  if(CMAKE_MATCH_1 LESS 1)
    message(FATAL_ERROR "AUD${ch} audible for less than 1 s")
  endif()
endforeach()

# Driver calls: "frame line event d0 caller".
file(STRINGS "${OUT}/audio_cues.txt" cues REGEX "^[0-9]")
set(title_music -1)
set(level_music -1)
set(fire_sfx 0)
foreach(c IN LISTS cues)
  string(REPLACE " " ";" f "${c}")
  list(GET f 0 frame)
  list(GET f 2 event)
  list(GET f 3 d0)
  if(event STREQUAL "music_init")
    if(title_music EQUAL -1)
      set(title_music ${frame})
    elseif(frame GREATER 1000 AND level_music EQUAL -1)
      set(level_music ${frame})
    endif()
  endif()
  if(event STREQUAL "sound_command" AND d0 MATCHES "^02" AND NOT frame LESS fire_frame)
    math(EXPR fire_sfx "${fire_sfx} + 1")
  endif()
endforeach()
message("title music_init frame ${title_music}, level music_init frame ${level_music}, sfx commands while firing ${fire_sfx}")
if(title_music LESS 0 OR title_music GREATER 10)
  message(FATAL_ERROR "title music not started in the first 10 frames")
endif()
if(level_music LESS 0)
  message(FATAL_ERROR "in-game music not started")
endif()
if(fire_sfx LESS 2)
  message(FATAL_ERROR "no sound effects while firing")
endif()

if(PYTHON)
  math(EXPR lv0 "${level_music} + 10")
  math(EXPR lv1 "${level_music} + 100")
  execute_process(
    COMMAND "${PYTHON}" -I "${CHECK}" "${OUT}/game_audio.wav" 20:400 ${lv0}:${lv1}
    RESULT_VARIABLE prc OUTPUT_VARIABLE pout ERROR_VARIABLE perr)
  message("${pout}${perr}")
  if(NOT prc EQUAL 0)
    message(FATAL_ERROR "WAV check failed")
  endif()
else()
  message("Python 3 not found: WAV content checks not run")
endif()
