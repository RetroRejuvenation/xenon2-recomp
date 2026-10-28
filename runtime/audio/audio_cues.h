// Logging of the game's music/sound-effect driver calls (for docs/re-notes/audio-notes.md and the CDTV
// music-cue mapping). Installs PC traps on the resident driver API at $19F9E-$19FF2 and on the shop/ending
// module's command entry $647A6 (only while $2094 points at it).
#pragma once
#include <cstdint>
#include <cstdio>

namespace x2 {

class Machine;

// One line per call: "frame line event d0 caller". The file must outlive the machine run.
void install_audio_cue_log(Machine& m, std::FILE* out);

} // namespace x2
