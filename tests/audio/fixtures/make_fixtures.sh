#!/bin/sh
# Regenerates the synthesised test tones in this directory (a 0.1 s sine, no game audio). Needs ffmpeg with the
# flac and libvorbis encoders. The commands are fixed and bit-exact; the sha256 of the results are in README.md.
#   FFMPEG=/path/to/ffmpeg sh make_fixtures.sh
set -eu
FFMPEG=${FFMPEG:-ffmpeg}
OUT=$(cd "$(dirname "$0")" && pwd)
common="-hide_banner -loglevel error -y -fflags +bitexact -flags:a +bitexact -map_metadata -1"
tone() { # rate channels frequency
  echo "sine=frequency=$3:sample_rate=$1:duration=0.1"
}
# mono, 22,050 Hz, 16-bit FLAC; stereo, 44,100 Hz, 24-bit FLAC
$FFMPEG -f lavfi -i "$(tone 22050 1 440)" $common -ac 1 -c:a flac -sample_fmt s16 "$OUT/tone_mono_22050.flac"
$FFMPEG -f lavfi -i "$(tone 44100 2 880)" $common -ac 2 -c:a flac -sample_fmt s32 -bits_per_raw_sample 24 "$OUT/tone_stereo_44100_24bit.flac"
# FLAC with loop tags (LOOPSTART=100, LOOPLENGTH=1000)
$FFMPEG -f lavfi -i "$(tone 22050 1 440)" $common -ac 1 -c:a flac -sample_fmt s16 -metadata LOOPSTART=100 -metadata LOOPLENGTH=1000 "$OUT/tone_loop_22050.flac"
# Ogg Vorbis: mono 22,050 Hz, stereo 44,100 Hz, and mono with loop tags (LOOPSTART=100, LOOPEND=1500)
$FFMPEG -f lavfi -i "$(tone 22050 1 440)" $common -ac 1 -c:a libvorbis -q:a 3 "$OUT/tone_mono_22050.ogg"
$FFMPEG -f lavfi -i "$(tone 44100 2 880)" $common -ac 2 -c:a libvorbis -q:a 3 "$OUT/tone_stereo_44100.ogg"
$FFMPEG -f lavfi -i "$(tone 22050 1 440)" $common -ac 1 -c:a libvorbis -q:a 3 -metadata LOOPSTART=100 -metadata LOOPEND=1500 "$OUT/tone_loop_22050.ogg"
