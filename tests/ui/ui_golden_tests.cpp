// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Golden renders of the demo pages. The kit draws with integer arithmetic into its own canvas, so a frame is
// the same on every platform: each scene is compared by hash at zero tolerance. No window, no SDL, no game data.
//
//   x2_ui_golden_tests HASHES [OUTDIR]     compare; on a mismatch write the actual picture to OUTDIR (PNG)
//   x2_ui_golden_tests --update HASHES     rewrite the hash file from the current output
//   x2_ui_golden_tests --dump DIR          write every scene as PNG (2x) for review, no comparison
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "demo/demo_scenes.h"

using namespace x2::ui;
using namespace x2::ui::demo;

namespace {

struct Variant {
  int w, h;
  bool hc;
  const char* tag;
};
const Variant kVariants[] = {{640, 360, false, "640x360"}, {480, 270, false, "480x270"}, {640, 360, true, "640x360hc"},
                             {480, 270, true, "480x270hc"}};

std::string hex64(uint64_t v) {
  char b[17];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
  return b;
}

}  // namespace

int main(int argc, char** argv) {
  bool update = false;
  std::string hashes, outdir, dump_all;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--update") update = true;
    else if (a == "--dump" && i + 1 < argc) dump_all = argv[++i];
    else if (hashes.empty()) hashes = a;
    else outdir = a;
  }

  std::map<std::string, std::pair<Canvas, uint64_t>> rendered;
  for (const SceneSpec& s : scenes())
    for (const Variant& v : kVariants) {
      Canvas c = render_scene(s, v.w, v.h, v.hc);
      const uint64_t h = c.hash();
      rendered.emplace(std::string(s.name) + "_" + v.tag, std::make_pair(std::move(c), h));
    }

  if (!dump_all.empty()) {
    for (const auto& [name, r] : rendered)
      if (!write_png(r.first, dump_all + "/" + name + ".png", 2)) {
        std::fprintf(stderr, "cannot write %s/%s.png\n", dump_all.c_str(), name.c_str());
        return 1;
      }
    std::printf("wrote %zu pictures to %s\n", rendered.size(), dump_all.c_str());
    return 0;
  }
  if (hashes.empty()) {
    std::fprintf(stderr, "usage: x2_ui_golden_tests HASHES [OUTDIR] | --update HASHES | --dump DIR\n");
    return 2;
  }
  if (update) {
    std::ofstream f(hashes, std::ios::trunc);
    f << "# Golden hashes of the UI demo scenes (tests/ui/ui_golden_tests.cpp). One line per scene: name, width, height, FNV-1a 64 of the canvas.\n"
         "# Regenerate with: x2_ui_golden_tests --update tests/ui/golden/hashes.txt, then review the pictures with --dump.\n";
    for (const auto& [name, r] : rendered) f << name << ' ' << r.first.width() << ' ' << r.first.height() << ' ' << hex64(r.second) << '\n';
    std::printf("updated %s (%zu scenes)\n", hashes.c_str(), rendered.size());
    return f ? 0 : 1;
  }

  std::map<std::string, std::string> expected;
  {
    std::ifstream f(hashes);
    if (!f) {
      std::fprintf(stderr, "cannot read %s\n", hashes.c_str());
      return 1;
    }
    std::string line;
    while (std::getline(f, line)) {
      if (line.empty() || line[0] == '#') continue;
      std::istringstream ls(line);
      std::string name, w, h, hash;
      ls >> name >> w >> h >> hash;
      expected[name] = hash;
    }
  }
  int bad = 0;
  for (const auto& [name, r] : rendered) {
    const auto it = expected.find(name);
    const std::string got = hex64(r.second);
    if (it == expected.end() || it->second != got) {
      ++bad;
      std::fprintf(stderr, "MISMATCH %s: expected %s, got %s\n", name.c_str(), it == expected.end() ? "(none)" : it->second.c_str(), got.c_str());
      if (!outdir.empty()) write_png(r.first, outdir + "/" + name + ".png", 2);
    }
  }
  for (const auto& [name, hash] : expected)
    if (!rendered.count(name)) {
      ++bad;
      std::fprintf(stderr, "STALE golden %s\n", name.c_str());
    }
  if (bad) {
    std::fprintf(stderr, "%d of %zu golden scenes differ (update with --update after reviewing the pictures)\n", bad, rendered.size());
    return 1;
  }
  std::printf("all %zu golden scenes match\n", rendered.size());
  return 0;
}
