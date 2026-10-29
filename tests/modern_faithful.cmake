# The Faithful switch: the presented pictures are the rendered frames themselves. With --faithful (also when a
# modern filter and interpolation are asked for as well) every picture the presenter gives for frames F..G hashes
# like the frame the video model rendered. Exits 77 (CTest "Skipped") without the user's assets.
# Usage: cmake -DX2RECOMP=<exe> -DASSETS=<dir> -DREPLAY=<script> -P modern_faithful.cmake
if(NOT EXISTS "${ASSETS}/game_0400.bin" OR NOT EXISTS "${ASSETS}/disc2.bin")
  message("SKIPPED: no game data in ${ASSETS}")
  cmake_language(EXIT 77)
endif()
foreach(extra "--faithful" "--faithful;--filter;crt;--modern;interp,scale=4;--aspect")
  execute_process(
    COMMAND "${X2RECOMP}" --headless --assets "${ASSETS}" --no-reglog --report-every 0 --input-script "${REPLAY}"
            --frames 1500 ${extra} --modern rate=50 --modern-out 1150-1450:- --hash-every 1
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "x2recomp exited with ${rc}: ${err}")
  endif()
  string(REGEX MATCHALL "presenthash [0-9]+ [0-9]+ [0-9a-f]+" presents "${out}")
  list(LENGTH presents n)
  if(n LESS 300)
    message(FATAL_ERROR "${extra}: only ${n} presented pictures")
  endif()
  foreach(p IN LISTS presents)
    string(REGEX MATCH "presenthash [0-9]+ ([0-9]+) ([0-9a-f]+)" _ "${p}")
    set(f ${CMAKE_MATCH_1})
    set(h ${CMAKE_MATCH_2})
    if(NOT out MATCHES "framehash ${f} ${h}")
      message(FATAL_ERROR "${extra}: picture '${p}' differs from the rendered frame ${f}")
    endif()
  endforeach()
  message("${extra}: ${n} presented pictures identical to the rendered frames")
endforeach()
