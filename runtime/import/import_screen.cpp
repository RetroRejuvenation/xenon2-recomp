// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "import/import_screen.h"

#include "gfx/sdl_display.h"
#include "import/importer.h"
#include "import/variants.h"

#include <cstdio>
#include <vector>

namespace x2 {

namespace {
constexpr uint32_t kWhite = 0xFFFFFF, kGrey = 0x9AA0A6, kTitle = 0x6FC3FF, kGreen = 0x7CDC7C, kYellow = 0xF2D45C,
                   kRed = 0xFF6B6B;
}

bool run_import_screen(SdlDisplay& display, const std::string& assets_dir, const std::string& reason) {
    using Line = SdlDisplay::TextLine;
    std::vector<std::string> dropped;
    std::vector<Line> log;
    bool done = false, drop_open = false;
    // The opt-in protection patch (variants.h): offered after a retail disc 1 was refused for lacking its key track,
    // and only applied after an explicit second key press that follows the disclaimer.
    bool patch_offer = false, patch_confirm = false;
    uint64_t last_drop_ns = 0, done_ns = 0;
    std::printf("x2recomp: no imported game data (%s); waiting for disk images to be dropped on the window\n",
                reason.c_str());

    auto run_import = [&](bool patch = false) {
        log.clear();
        patch_offer = patch_confirm = false;
        import::ImportOptions opt;
        opt.apply_protection_patch = patch;
        opt.confirmed_ownership = patch; // only ever true after the explicit confirmation key below
        import::Result r = import::import_paths(dropped, opt);
        for (const import::Message& m : r.messages)
            log.push_back({import::format_message(m), m.level == import::Message::Error  ? kRed
                                                      : m.level == import::Message::Warn ? kYellow
                                                                                         : kGrey});
        for (const import::Message& m : r.messages) std::printf("%s\n", import::format_message(m).c_str());
        if (r.status == import::Status::Ok) {
            std::string err;
            if (import::write_assets(assets_dir, r, err)) {
                log.push_back({"Imported. Starting the game...", kGreen});
                std::printf("imported into %s\n", assets_dir.c_str());
                done = true;
                done_ns = SDL_GetTicksNS();
            } else {
                log.push_back({"error: " + err, kRed});
            }
            dropped.clear();
            return;
        }
        for (const import::Message& m : r.messages)
            if (m.level == import::Message::Error && m.text.find("--apply-protection-patch") != std::string::npos) patch_offer = true;
        if (patch_offer) {
            log.push_back({"This is a retail disc 1 from an image without the key track. Best: import an IPF or flux dump.", kWhite});
            log.push_back({"Or press P to read about the opt-in protection patch (nothing is changed until you confirm).", kWhite});
            return; // keep what was dropped
        }
        // Keep what was dropped when only one disc was given, so the other one can be dropped next.
        bool missing_only = true;
        for (const import::Message& m : r.messages)
            if (m.level == import::Message::Error && m.text.rfind("disc ", 0) != 0) missing_only = false;
        if (missing_only) log.push_back({"Now drop the other disc as well (Backspace forgets the dropped files).", kWhite});
        else dropped.clear();
    };

    while (true) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: return false;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.scancode == SDL_SCANCODE_ESCAPE || e.key.scancode == SDL_SCANCODE_F10 ||
                    (e.key.scancode == SDL_SCANCODE_Q && (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI))))
                    return false;
                if (e.key.scancode == SDL_SCANCODE_BACKSPACE) { dropped.clear(); log.clear(); patch_offer = patch_confirm = false; }
                if (patch_offer && !patch_confirm && e.key.scancode == SDL_SCANCODE_P) patch_confirm = true;
                else if (patch_confirm && e.key.scancode == SDL_SCANCODE_N) patch_confirm = false;
                else if (patch_confirm && e.key.scancode == SDL_SCANCODE_Y && !dropped.empty()) run_import(true);
                if (e.key.scancode == SDL_SCANCODE_F11 ||
                    (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)))
                    display.toggle_fullscreen();
                break;
            case SDL_EVENT_DROP_BEGIN: drop_open = true; break;
            case SDL_EVENT_DROP_FILE:
                if (e.drop.data && !done) dropped.push_back(e.drop.data);
                last_drop_ns = SDL_GetTicksNS();
                break;
            case SDL_EVENT_DROP_COMPLETE:
                drop_open = false;
                if (!dropped.empty() && !done) run_import();
                last_drop_ns = 0;
                break;
            default: break;
            }
        }
        uint64_t now = SDL_GetTicksNS();
        // Platforms that send no DROP_COMPLETE: import 300 ms after the last dropped file.
        if (!done && !drop_open && last_drop_ns && now - last_drop_ns > 300'000'000 && !dropped.empty()) {
            last_drop_ns = 0;
            run_import();
        }
        if (done && now - done_ns > 1'500'000'000) return true;

        std::vector<Line> lines = {
            {"XENON 2 MEGABLAST - import your disk images", kTitle},
            {"", kWhite},
            {"No game data has been imported yet (" + reason + ").", kWhite},
            {"This project ships no game data: you need your own copy.", kWhite},
            {"", kWhite},
            {"Drag and drop onto this window: both disk images (.adf, .adz or .dms), a .zip holding them, or a folder.",
             kWhite},
            {"Supported: the two-disc BS1 [t+3] release, and the retail 2-disc release from IPF or flux images (SCP, "
             "KryoFlux, HFE). Single-disk cracks (PNA, QTX, Black Monks, VF) are not.",
             kGrey},
            {"Command line alternative: x2recomp --import PATH [PATH2]", kGrey},
            {"Data folder: " + assets_dir, kGrey},
            {"Esc: quit", kGrey},
            {"", kWhite},
        };
        for (const std::string& d : dropped) lines.push_back({"dropped: " + d, kWhite});
        lines.insert(lines.end(), log.begin(), log.end());
        if (patch_confirm) {
            lines.push_back({"", kWhite});
            for (const std::string& l : import::protection_patch_disclaimer()) lines.push_back({l, kYellow});
            lines.push_back({"Press Y to confirm that you own the original disc and apply the patch, or N to cancel.", kWhite});
        }
        display.present_text(lines);
        SDL_Delay(16);
    }
}

} // namespace x2
