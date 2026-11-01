// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "modern/screen_pack.h"


#include <algorithm>

namespace x2::modern {

const pack::Picture* ScreenPackLibrary::add_picture(pack::Picture p) {
    pictures_.push_back(std::move(p));
    return &pictures_.back();
}

const pack::ScreenEntry* ScreenPackLibrary::add_screen(pack::ScreenEntry e) {
    by_block_valid_ = false;
    for (const pack::ScreenEntry*& s : screens_)
        if (s->name == e.name) {
            entries_.push_back(std::move(e));
            s = &entries_.back();
            return s;
        }
    entries_.push_back(std::move(e));
    screens_.push_back(&entries_.back());
    return screens_.back();
}

const std::vector<const pack::ScreenEntry*>& ScreenPackLibrary::screens_for_block(uint64_t first_block_hash) const {
    if (!by_block_valid_) {
        by_block_.clear();
        for (const pack::ScreenEntry* e : screens_)
            if (!e->signature.blocks.empty()) by_block_[e->signature.blocks[0].hash].push_back(e);
        by_block_valid_ = true;
    }
    static const std::vector<const pack::ScreenEntry*> none;
    auto it = by_block_.find(first_block_hash);
    return it == by_block_.end() ? none : it->second;
}

} // namespace x2::modern
