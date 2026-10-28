// x2recomp - Xenon 2 Megablast runtime. Phase 3a: headless, interpreter-driven.
//
//   x2recomp --headless --frames N --dump-dir DIR [--assets DIR] [options]
//
// See docs/BUILDING.md for all options and the output format.
#include "boot.h"
#include "debug_dump.h"
#include "disk_shim.h"
#include "machine.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "m68k.h"
}

namespace {

struct InputEvent {
    uint32_t frame;
    enum Kind { Key, Fire } kind;
    uint8_t code;
    uint32_t hold;
};

struct Options {
    bool headless = false;
    uint32_t frames = 500;
    std::string dump_dir;
    std::string assets = "generated/assets";
    uint32_t report_every = 50;
    uint32_t ppm_every = 0;
    bool reglog = true;
    std::vector<uint32_t> trace_frames;
    std::vector<InputEvent> input;
};

void usage() {
    std::fprintf(stderr,
                 "usage: x2recomp --headless --frames N --dump-dir DIR [options]\n"
                 "  --assets DIR        directory with game_0400.bin and disc2.bin (default generated/assets)\n"
                 "  --report-every N    status report interval in frames (default 50)\n"
                 "  --ppm-every N       also dump the bitplane framebuffer every N frames\n"
                 "  --no-reglog         do not write per-frame register write logs\n"
                 "  --trace-frame F     write a disassembly trace of every instruction in frame F\n"
                 "  --key F:CODE        press Amiga raw key CODE (hex) at frame F (released 2 frames later)\n"
                 "  --fire F[:N]        hold joystick fire from frame F for N frames (default 3)\n");
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--headless") o.headless = true;
        else if (a == "--frames") { auto v = next(); if (!v) return false; o.frames = uint32_t(std::strtoul(v, nullptr, 0)); }
        else if (a == "--dump-dir") { auto v = next(); if (!v) return false; o.dump_dir = v; }
        else if (a == "--assets") { auto v = next(); if (!v) return false; o.assets = v; }
        else if (a == "--report-every") { auto v = next(); if (!v) return false; o.report_every = uint32_t(std::strtoul(v, nullptr, 0)); }
        else if (a == "--ppm-every") { auto v = next(); if (!v) return false; o.ppm_every = uint32_t(std::strtoul(v, nullptr, 0)); }
        else if (a == "--no-reglog") o.reglog = false;
        else if (a == "--trace-frame") { auto v = next(); if (!v) return false; o.trace_frames.push_back(uint32_t(std::strtoul(v, nullptr, 0))); }
        else if (a == "--key" || a == "--fire") {
            auto v = next();
            if (!v) return false;
            char* end = nullptr;
            InputEvent e{uint32_t(std::strtoul(v, &end, 0)), a == "--key" ? InputEvent::Key : InputEvent::Fire, 0, 3};
            if (*end == ':') {
                if (e.kind == InputEvent::Key) e.code = uint8_t(std::strtoul(end + 1, nullptr, 16));
                else e.hold = uint32_t(std::strtoul(end + 1, nullptr, 0));
            } else if (e.kind == InputEvent::Key) return false;
            o.input.push_back(e);
        } else if (a == "--help" || a == "-h") return false;
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
    }
    return true;
}

// Names for the most common PCs in reports (docs/re-notes/ghidra-code-map.md).
const char* known_name(uint32_t pc) {
    static const std::map<uint32_t, const char*> names = {
        {0x0FAC, "irq2_cia_keyboard"}, {0x129A, "blit_bob_cookie"}, {0x15CE, "print_string_blit"},
        {0x17D6, "blit_objects_main"}, {0x18B6, "draw_background_a"}, {0x1BD0, "draw_background_b"},
        {0x2026, "wait_frame_swap_copper"}, {0x202E, "wait_frame_swap_copper_alt"}, {0x2098, "irq3_vbl_diskload"},
        {0x20C2, "irq3_vbl"}, {0x6372, "palette_fade"}, {0x7032, "reset_entry"}, {0x763C, "init"},
        {0x78A4, "main_loop"}, {0x7940, "key_dispatch"}, {0x8F9A, "disk_check_discid"},
        {0x8FF2, "level_load_and_run"}, {0x917E, "level_load_if_changed"}, {0x921A, "disk_read_sectors"},
        {0x95E4, "irq1_dskblk"}, {0x19FAC, "music_tick"}, {0x1A154, "music_tick_core"},
    };
    const char* best = "";
    uint32_t best_addr = 0;
    for (auto& [a, n] : names)
        if (a <= pc && a >= best_addr && pc - a < 0x200) { best = n; best_addr = a; }
    return best;
}

struct Milestones {
    long cop1lc = -1, copper_running = -1, bitplanes = -1, vbl_handler = -1, disc_check_ok = -1, main_loop = -1;
};

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) { usage(); return 2; }
    if (!opt.headless) {
        std::fprintf(stderr, "x2recomp: only --headless is implemented in Phase 3a (no renderer yet)\n");
        usage();
        return 2;
    }
    if (opt.dump_dir.empty()) { usage(); return 2; }

    x2::GameAssets assets;
    std::string err;
    if (!x2::load_game_assets(opt.assets, assets, err)) {
        std::fprintf(stderr, "x2recomp: %s\n", err.c_str());
        return 3;
    }
    namespace fs = std::filesystem;
    fs::create_directories(opt.dump_dir);
    if (opt.reglog) fs::create_directories(opt.dump_dir + "/regs");

    x2::Machine m;
    x2::DiskShim disk(assets.disc2);
    disk.install(m);
    Milestones ms;
    uint32_t vbl_entries = 0, main_loop_entries = 0, discid_ok = 0, discid_calls = 0;
    // Entry-count probes; PC traps run before the instruction, then execution continues normally.
    m.add_trap(0x20C2, [&](x2::Machine&) { ++vbl_entries; });
    // $78CE (addq.w #1,$CDC): the main loop's back-edge target, reached once per game-logic frame.
    m.add_trap(0x78CE, [&](x2::Machine&) { ++main_loop_entries; });
    // $8FDE: cmpi.l #'X2B ',$49602 right after the shim returns inside the disc-id check.
    m.add_trap(0x8FDE, [&](x2::Machine& mm) {
        ++discid_calls;
        if (mm.read32(0x49602) == 0x58324220) ++discid_ok;
    });
    x2::boot_game(m, assets);
    m.log_writes = opt.reglog;

    FILE* trace_file = nullptr;
    std::map<uint32_t, uint32_t> window_pcs;
    std::array<uint32_t, 0x100> window_reads{};
    std::array<uint32_t, 32> window_cia{};
    uint32_t window_start = 0;

    std::printf("x2recomp headless: %u frames, %d lines x %d CPU cycles per frame\n", opt.frames,
                x2::kLinesPerFrame, x2::kCpuCyclesPerLine);
    for (uint32_t f = 0; f < opt.frames; ++f) {
        // Scripted input for this frame.
        bool fire = false;
        for (const InputEvent& e : opt.input) {
            if (e.kind == InputEvent::Key) {
                if (e.frame == f) m.key_event(e.code, false);
                if (e.frame + 2 == f) m.key_event(e.code, true);
            } else if (f >= e.frame && f < e.frame + e.hold) fire = true;
        }
        m.set_joystick(false, false, false, false, fire);

        bool tracing = std::find(opt.trace_frames.begin(), opt.trace_frames.end(), f) != opt.trace_frames.end();
        if (tracing) {
            trace_file = std::fopen((opt.dump_dir + "/trace_" + std::to_string(f) + ".txt").c_str(), "w");
            m.trace = [&](x2::Machine& mm, uint32_t pc) {
                char buf[128];
                m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
                std::fprintf(trace_file, "%3d %06X  %-36s d0=%08X d1=%08X a0=%08X a7=%08X sr=%04X\n", mm.line, pc, buf,
                             mm.dreg(0), mm.dreg(1), mm.areg(0), mm.areg(7), mm.sr());
            };
        }
        m.frame_writes.clear();
        m.pc_samples.clear();
        m.custom_reads.fill(0);
        m.cia_reads.fill(0);
        m.run_frame();
        if (tracing) { m.trace = nullptr; std::fclose(trace_file); trace_file = nullptr; }

        for (auto& [pc, n] : m.pc_samples) window_pcs[pc] += n;
        for (int i = 0; i < 0x100; ++i) window_reads[size_t(i)] += m.custom_reads[size_t(i)];
        for (int i = 0; i < 32; ++i) window_cia[size_t(i)] += m.cia_reads[size_t(i)];

        if (opt.reglog)
            x2::write_reglog(m, opt.dump_dir + "/regs/frame_" + std::string(5 - std::min<size_t>(5, std::to_string(f).size()), '0') +
                                    std::to_string(f) + ".log");
        if (opt.ppm_every && f % opt.ppm_every == 0)
            x2::write_bitplane_ppm(m, opt.dump_dir + "/frame_" + std::to_string(f) + ".ppm");

        // Milestones.
        bool cop_set = m.regs[x2::reg::COP1LCH >> 1] || m.regs[(x2::reg::COP1LCH >> 1) + 1];
        if (ms.cop1lc < 0 && cop_set) ms.cop1lc = f;
        if (ms.copper_running < 0 && m.copper_moves) ms.copper_running = f;
        if (ms.bitplanes < 0 && m.display.valid && ((m.display.bplcon0 >> 12) & 7) &&
            (m.display.dmacon & 0x0300) == 0x0300 && m.display.bplpt[0])
            ms.bitplanes = f;
        if (ms.vbl_handler < 0 && vbl_entries) ms.vbl_handler = f;
        if (ms.disc_check_ok < 0 && discid_ok) ms.disc_check_ok = f;
        if (ms.main_loop < 0 && main_loop_entries) ms.main_loop = f;

        if (opt.report_every && (f + 1) % opt.report_every == 0) {
            std::vector<std::pair<uint32_t, uint32_t>> pcs(window_pcs.begin(), window_pcs.end());
            std::sort(pcs.begin(), pcs.end(), [](auto& a, auto& b) { return a.second > b.second; });
            uint32_t total = 0;
            for (auto& p : pcs) total += p.second;
            std::printf("[frames %u-%u] pc=%06X sr=%04X intena=%04X intreq=%04X dmacon=%04X blits=%u copmoves=%u "
                        "disk=%u/%u sectors vbl=%u mainloop=%u restarts=%u unmapped=%u\n",
                        window_start, f, m.pc(), m.sr(), m.intena, m.intreq, m.dmacon, m.blits, m.copper_moves,
                        disk.calls, disk.sectors_read, vbl_entries, main_loop_entries,
                        m.restarts ? m.restarts - 1 : 0, m.unmapped_accesses);
            std::printf("   hot PCs:");
            for (size_t i = 0; i < pcs.size() && i < 5; ++i)
                std::printf(" %06X %s(%.0f%%)", pcs[i].first, known_name(pcs[i].first), 100.0 * pcs[i].second / std::max(1u, total));
            std::printf("\n   polled:");
            std::vector<std::pair<uint32_t, std::string>> polls;
            for (int i = 0; i < 0x100; ++i)
                if (window_reads[size_t(i)]) polls.push_back({window_reads[size_t(i)], x2::reg::name(uint16_t(i * 2))});
            static const char* const cia_names[16] = {"PRA", "PRB", "DDRA", "DDRB", "TALO", "TAHI", "TBLO", "TBHI",
                                                       "TODLO", "TODMID", "TODHI", "R0B", "SDR", "ICR", "CRA", "CRB"};
            for (int i = 0; i < 32; ++i)
                if (window_cia[size_t(i)]) polls.push_back({window_cia[size_t(i)], std::string(i < 16 ? "CIAA_" : "CIAB_") + cia_names[i & 15]});
            std::sort(polls.begin(), polls.end(), [](auto& a, auto& b) { return a.first > b.first; });
            for (size_t i = 0; i < polls.size() && i < 6; ++i) std::printf(" %s x%u", polls[i].second.c_str(), polls[i].first);
            std::printf("\n");
            std::fflush(stdout);
            window_pcs.clear();
            window_reads.fill(0);
            window_cia.fill(0);
            window_start = f + 1;
        }
    }

    x2::write_chip_ram(m, opt.dump_dir + "/chipram.bin");
    x2::write_bitplane_ppm(m, opt.dump_dir + "/final.ppm");
    std::printf("milestones (frame numbers, -1 = never):\n"
                "  first VBL handler entry ($20C2)       %ld\n"
                "  COP1LC first programmed               %ld\n"
                "  copper first executing MOVEs          %ld\n"
                "  bitplanes first displayed (DMA+BPU)   %ld\n"
                "  disc-id check passed ('X2B ')         %ld (%u/%u checks ok)\n"
                "  main loop $78A4 first reached         %ld (%u logic frames)\n",
                ms.vbl_handler, ms.cop1lc, ms.copper_running, ms.bitplanes, ms.disc_check_ok, discid_ok, discid_calls,
                ms.main_loop, main_loop_entries);
    std::printf("disk shim: %u calls, %u sectors, %u bad; last sector=%u count=%u dest=%06X\n", disk.calls,
                disk.sectors_read, disk.bad_requests, disk.last_sector, disk.last_count, disk.last_dest);
    std::printf("blits=%u (line mode, unimplemented: %u) restarts-via-$7032=%u (last from %06X) unmapped=%u (last %06X)\n",
                m.blits, m.line_blits, m.restarts ? m.restarts - 1 : 0, m.last_restart_from, m.unmapped_accesses,
                m.last_unmapped_addr);
    std::printf("outputs: %s/{chipram.bin,final.ppm%s}\n", opt.dump_dir.c_str(), opt.reglog ? ",regs/frame_NNNNN.log" : "");
    return 0;
}
