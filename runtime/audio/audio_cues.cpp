#include "audio_cues.h"

#include "machine.h"

namespace x2 {

void install_audio_cue_log(Machine& m, std::FILE* out) {
    std::fprintf(out, "# frame line event d0 caller(return address)\n");
    struct Entry {
        uint32_t addr;
        const char* name;
        uint32_t guard_var; // 0 = always; else only when read32(guard_var) == addr
    };
    static const Entry entries[] = {
        {0x19F9E, "music_init", 0},     {0x19FBA, "music_stop", 0},  {0x19FC8, "music_dma_on", 0},
        {0x19FD6, "sound_command", 0},  {0x19FE4, "music_api_19fe4", 0},  {0x19FF2, "music_api_19ff2", 0},
        {0x647A6, "slot6_command", 0x2094},
    };
    for (const Entry& e : entries) {
        m.add_trap(e.addr, [e, out](Machine& mm) {
            if (e.guard_var && mm.read32(e.guard_var) != e.addr) return;
            std::fprintf(out, "%u %d %s %04X %06X\n", mm.frame, mm.line, e.name, mm.dreg(0) & 0xFFFF,
                         mm.read32(mm.areg(7)) & 0xFFFFFF);
        });
    }
}

} // namespace x2
