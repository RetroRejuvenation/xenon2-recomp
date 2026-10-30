Licences in this package
========================

x2recomp itself is free software under the GNU General Public License, version 3 or later
(GPL-3.0-or-later). The full licence text is in the LICENSE file next to README.md.

Part of the accurate-timing code is derived from WinUAE/Amiberry, which is GPL. That is why x2recomp is
GPL and not a permissive licence. PROVENANCE.md (next to README.md) records exactly which code this is,
how closely it follows the original, and the licence of each source.

This package contains no game data. Xenon 2 Megablast itself (its code, graphics, music and text) is
copyright The Bitmap Brothers / Mirrorsoft, is NOT included here, and is not covered by any licence in
this package. x2recomp runs only from disk images you own (x2recomp --import). CDTV CD audio likewise
comes from your own disc image and is never shipped.

Third-party components keep their own licences. All of them are compatible with GPL-3.0-or-later:

  Musashi-LICENSE.txt    Musashi 680x0 emulator (MIT-style)
  puff-LICENSE.txt       puff inflate (zlib licence)
  SDL3-LICENSE.txt       SDL 3 (zlib licence), when this package bundles SDL3

SoftFloat is deliberately absent. Musashi can use SoftFloat for its FPU, but this build does not compile,
link or ship any of it: the game is a 68000 with no FPU, so runtime/musashi_no_fpu.c stands in for it.

THIRD-PARTY-NOTICES.md (next to README.md) lists these components with versions and the compatibility
reasoning.
