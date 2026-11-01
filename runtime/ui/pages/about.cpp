// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "about.h"

#include <string>

#include "page_common.h"

namespace x2::ui {

void page_about(Context& ctx, const AboutInfo& info, Rect body) {
  const Theme& t = ctx.theme;
  page_title(ctx, body, "About", "");
  ScrollView sv = ctx.scroll_begin("about", body);
  Stack s(body.x, sv.origin_y() + 2, body.w - 6, 4);

  draw_label(ctx, s.row(18), "x2recomp " + (info.version.empty() ? std::string("(development build)") : info.version), t.c.amber);
  if (!info.host.build_id.empty()) draw_caption(ctx, s.row(10), "build " + info.host.build_id, t.c.dim);
  if (!info.host.platform.empty()) draw_caption(ctx, s.row(10), "platform " + info.host.platform, t.c.dim);
  {
    std::string sdl = info.host.sdl_version.empty() ? std::string("SDL: not available") : "SDL " + info.host.sdl_version;
    if (!info.host.video_driver.empty()) sdl += ", video " + info.host.video_driver;
    if (!info.host.audio_driver.empty()) sdl += ", audio " + info.host.audio_driver;
    draw_caption(ctx, s.row(10), sdl, t.c.dim);
  }
  s.space(4);

  section_header(ctx, s.row(12), "Licence");
  paragraph(ctx, s, "Free software under the GNU General Public License, version 3 or later. The source code is on the project page.", t.c.text);
  const Rect links = s.row(22);
  int x = links.x;
  if (info.open_url) {
    const int w = text_width("Open the project page") + 20;
    if (button(ctx, "Open the project page", {x, links.y, w, 22})) info.open_url(info.project_url);
    x += w + 8;
  }
  if (info.open_data_folder) {
    const int w = text_width("Open the data folder") + 20;
    if (button(ctx, "Open the data folder", {x, links.y, w, 22})) info.open_data_folder();
  }
  s.space(4);

  section_header(ctx, s.row(12), "What this is");
  paragraph(ctx, s, "This project contains no game data. You need your own copy of the game, which you give to the importer; nothing is uploaded "
                 "or shared. The game belongs to its rights holders. This program only runs the copy you provide, and it ships no cracks, "
                 "no patched images and no assets.",
                 t.c.text);
  s.space(4);

  section_header(ctx, s.row(12), "Third-party software");
  for (const ThirdParty& p : third_party()) {
    draw_label(ctx, s.row(16), std::string(p.name) + "  (" + p.licence + ")", t.c.text);
    draw_caption(ctx, s.row(10), p.use, t.c.dim);
  }
  paragraph(ctx, s, "The full licence texts ship with every release (the licenses folder and THIRD-PARTY-NOTICES.md).", t.c.dim);
  s.space(4);

  section_header(ctx, s.row(12), "Help and problems");
  paragraph(ctx, s, "The documentation is in the docs folder of the release and on the project page. To report a problem, use Copy diagnostics on "
                 "the Advanced page and open an issue on the project page.",
                 t.c.dim);
  s.space(4);

  section_header(ctx, s.row(12), "Access");
  paragraph(ctx, s, "Everything here works with a keyboard or a gamepad alone. Text size, Contrast and Motion are on the Display page. Screen "
                 "readers are not supported because the pages are drawn by the program itself; the command line (--help) is the accessible way "
                 "to do all of this.",
                 t.c.dim);
  ctx.scroll_end(sv, s.used() + 8);
}

}  // namespace x2::ui
