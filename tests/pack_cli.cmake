# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Asset packs v2, workstream 1a: the new command-line flags are parsed, documented in --help, and the ones that need later
# workstreams say "not implemented yet" with exit code 3 instead of being ignored. No game data needed.
# Usage: cmake -DX2RECOMP=<exe> -P pack_cli.cmake
function(run name expect_rc expect_text)
  execute_process(COMMAND "${X2RECOMP}" ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL ${expect_rc})
    message(FATAL_ERROR "${name}: expected exit code ${expect_rc}, got ${rc}\n${out}${err}")
  endif()
  if(NOT "${expect_text}" STREQUAL "")
    if(NOT "${out}${err}" MATCHES "${expect_text}")
      message(FATAL_ERROR "${name}: output does not match '${expect_text}':\n${out}${err}")
    endif()
  endif()
endfunction()

# --help documents every new flag and the new --modern items.
execute_process(COMMAND "${X2RECOMP}" --help RESULT_VARIABLE rc OUTPUT_VARIABLE help ERROR_VARIABLE help_err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "--help failed (${rc})")
endif()
foreach(flag "--check-asset-pack" "--list-asset-packs" "--dump-audio" "--speech-volume" "--against" "--strict" "--full" "noaudio")
  string(FIND "${help}" "${flag}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "--help does not mention ${flag}")
  endif()
endforeach()

# The author tools are not implemented yet: exit code 3 and a message, whatever the options.
run(check 3 "not implemented yet" --check-asset-pack some-pack)
run(check_all_options 3 "not implemented yet" --check-asset-pack some-pack --game cdtv --build cdtv --against dump --full --strict --json)
run(list 3 "not implemented yet" --list-asset-packs --headless)
run(list_json 3 "not implemented yet" --list-asset-packs --json --headless)
run(dump_audio 3 "not implemented yet" --dump-audio some-dir --headless)

# Options that only mean something with --check-asset-pack are refused without it.
run(against_alone 2 "only apply to --check-asset-pack" --against dump)
run(build_alone 2 "only apply to --check-asset-pack" --build cdtv)
run(full_alone 2 "only apply to --check-asset-pack" --full)
run(strict_alone 2 "only apply to --check-asset-pack" --strict)
run(json_alone 2 "--json only applies" --json)

# The state flags are accepted: --modern audio|noaudio, --speech-volume (with a note that nothing applies it yet), repeated
# --asset-pack. --print-data-dir exits right after parsing.
run(modern_audio 0 "" --modern audio --print-data-dir)
run(modern_noaudio 0 "" --modern noaudio,upgrade --print-data-dir)
run(speech_volume 0 "speech-volume is stored" --speech-volume 50 --print-data-dir)
run(asset_pack_twice 0 "" --asset-pack a --asset-pack b --print-data-dir)
# Unknown --modern items and a missing value are still errors.
run(modern_unknown 2 "unknown item" --modern bogus --print-data-dir)
run(speech_missing_value 2 "" --speech-volume)
run(check_missing_path 2 "" --check-asset-pack)
