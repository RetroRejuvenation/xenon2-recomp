// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The launcher's frame: the home page with a card per release, the import flow, the settings pages (pages.h) and About. Pure drawing and
// input handling on a LauncherState; the window host (launcher.cpp) and the tests both call it.
#pragma once
#include "../launcher_state.h"

namespace x2::ui {

// One whole frame. Does not poll the state: the host calls LauncherState::poll() once per loop iteration, before the frame.
void launcher_frame(Context& ctx, LauncherState& st);

// The text of a family for the cards and the import view.
const char* family_noun(GameKind g);  // "Amiga disk images", "a CDTV disc image", "Atari ST disk images"

}  // namespace x2::ui
