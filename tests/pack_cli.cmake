# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Asset packs v2: the command-line flags are parsed and documented in --help, --check-asset-pack and --list-asset-packs
# (workstream 1b) run on small packs written here, and --dump-audio still says "not implemented yet" (workstream 3b). No game data needed.
# Usage: cmake -DX2RECOMP=<exe> -DWORK=<scratch dir> -P pack_cli.cmake
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

# --dump-audio needs the effect hooks of a later workstream: exit code 3 and a message.
run(dump_audio 3 "not implemented yet" --dump-audio some-dir --headless)

# --check-asset-pack: a path that does not exist is exit code 3, whatever the options.
run(check_missing 3 "no such asset pack" --check-asset-pack some-pack)
run(check_missing_options 3 "no such asset pack" --check-asset-pack some-pack --game cdtv --build cdtv --against dump --full --strict --json)

# ... and on small packs written here.
if(NOT WORK)
  message(FATAL_ERROR "pass -DWORK=<scratch dir>")
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
function(pack_dir name manifest)
  file(MAKE_DIRECTORY "${WORK}/${name}")
  file(WRITE "${WORK}/${name}/manifest.json" "${manifest}")
endfunction()
pack_dir(good "{\"format\": \"x2-asset-pack\", \"version\": 2, \"name\": \"Good\", \"license\": \"CC0-1.0\"}")
pack_dir(nolicense "{\"format\": \"x2-asset-pack\", \"version\": 2, \"name\": \"No licence\"}")
pack_dir(unknown "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"CC0-1.0\", \"colour\": 1}")
pack_dir(broken "{\"format\": \"x2-asset-pack\", \"version\": 2,")
pack_dir(future "{\"format\": \"x2-asset-pack\", \"version\": 9, \"license\": \"CC0-1.0\"}")
pack_dir(badentry "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"CC0-1.0\", \"graphics\": {\"not-a-key\": {\"png\": \"a.png\"}}}")
file(MAKE_DIRECTORY "${WORK}/empty-dir")
run(check_good 0 "valid" --check-asset-pack "${WORK}/good")
run(check_good_json 0 "\"summary\"" --check-asset-pack "${WORK}/good" --json)
run(check_nolicense 0 "warning" --check-asset-pack "${WORK}/nolicense")
run(check_nolicense_strict 2 "license" --check-asset-pack "${WORK}/nolicense" --strict)
run(check_unknown 0 "unknown field \"colour\"" --check-asset-pack "${WORK}/unknown")
run(check_unknown_strict 1 "unknown field" --check-asset-pack "${WORK}/unknown" --strict)
run(check_broken 2 "manifest.json:" --check-asset-pack "${WORK}/broken")
run(check_future 2 "needs a newer x2recomp" --check-asset-pack "${WORK}/future")
run(check_badentry 2 "the key must be" --check-asset-pack "${WORK}/badentry")
run(check_no_manifest 2 "missing file" --check-asset-pack "${WORK}/empty-dir")
run(check_good_cdtv 0 "valid" --check-asset-pack "${WORK}/good" --game cdtv --build cdtv)

# --list-asset-packs reads <data dir>/packs: ids, names, the problem of an unusable pack, enabled state, --json.
file(MAKE_DIRECTORY "${WORK}/data/packs")
file(COPY "${WORK}/good" DESTINATION "${WORK}/data/packs")
file(COPY "${WORK}/broken" DESTINATION "${WORK}/data/packs")
file(MAKE_DIRECTORY "${WORK}/data/packs/not-a-pack")
run(list 0 "good" --list-asset-packs --headless --data-dir "${WORK}/data")
run(list_problem 0 "problem: manifest.json:" --list-asset-packs --headless --data-dir "${WORK}/data")
run(list_json 0 "\"id\": \"good\"" --list-asset-packs --json --headless --data-dir "${WORK}/data")
run(list_empty 0 "No asset packs found" --list-asset-packs --headless --data-dir "${WORK}/nodata")

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
