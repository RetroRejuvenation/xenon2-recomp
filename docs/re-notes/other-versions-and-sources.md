# Other versions of Xenon 2 Megablast, and where development material might be

A desk survey (October 2026) of editions and ports that `docs/re-notes/sources.md`, `cracks.md`,
`single-disk-variants.md`, `cdtv.md` and (on the platform-assets branch) `platform-images.md` do not yet cover, and of
possible sources of original development material. **Metadata and links only: nothing was downloaded**, and no
copyrighted content is reproduced beyond short facts.

How to read the evidence. **Verified** = a page was fetched and says it. **Secondary** = a search result or an
aggregator says it and no primary source was seen. **Rumour** = said somewhere, unconfirmed. Web pages were treated
as untrusted data. Several sites (MobyGames, Hall of Light, the Atarimania dump pages, AtariAge forums, AMP, the Wikipedia
Jaguar references) returned 403 or an anti-bot page, so claims that depend only on them are marked **Secondary**.
Dates for the same release differ between sources (see section 2 note).

## 1. Short answer

- **New and valuable: real flux of a retail Amiga disk is on the Internet Archive** (item
  `amiga-original-xenon-2-powerplay`, a `.scp` of 36,514,264 bytes and a `.flux` of 28,810,078 bytes, described as
  "Xenon 2 for Amiga computers, original retail disk"). It is the first real (non-synthetic) flux of this game we know of,
  and a ready-made test of the SCP reader and the retail key-track path. Verified from the item's metadata only; which disk it
  holds (1 or 2) is not stated.
- **New: the Archimedes has a full retail version, not just the demo we hold.** Eclipse ported it for GamesWare (1993); an
  updated release by Stephen Harrison appeared on the Acorn User cover disc in January 1997 (all protection removed) and on
  Acorn User CD 3. The CD 3 images are on the Internet Archive (488 MB ISO `AUCD3.iso`, and a 429 MB zip).
- **New: Atari ST** is a large family we hold nothing of: Image Works 1989 (two disks, Pasti dumps), MCM (Spain, 1989), ST Format
  preview (1989, a different, earlier build), Mirror Image (1991), Kixx (1993, one double-sided disk), and a 2015 STe DMA-audio
  hack. The Amiga and ST games were written together, so the ST build is the nearest thing to a second reading of the same engine.
- **New: Amiga budget re-releases** exist from Mirror Image (1991) and Kixx (1993); these are probably the "budget v4" and
  "PDos v5" imager versions for which we have no image (unconfirmed link).
- **A CDTV review/promo CD-R** exists (sold by ami64.com): "a choice of five languages, including Xenitese" and 12 tracks. Our own
  notes say the disc has four languages (EN/FR/DE/IT); the fifth is worth checking against the disc.
- **Nothing found that is real development material** (no source, no assets, no masters) in public. The leads are people and a
  book, not files (section 5).
- **Unreleased:** a C64 conversion was discussed and never made; a Spectrum demo was rejected; a SNES port was announced and
  cancelled. No surviving builds are documented for any of them.
- **Rights:** Rebellion Developments bought "The Bitmap Brothers brand and portfolio" on 25 November 2019. The press release
  does not name Xenon 2 or say whether source code was part of the deal.

## 2. Editions and ports

| Platform / edition | Year, publisher | Evidence | Status for us | Why it matters |
|---|---|---|---|---|
| Amiga, Image Works retail (SPS 0297 / 2874) | 1989 | catalogued | known | the project's main target |
| Amiga, Power Pack (SPS 2234), Bombuzal + Xenon 2 (SPS 1762), "4 Others", LGCSS, demo | 1990-93 | catalogued | known | see `single-disk-variants.md` |
| **Amiga retail disk as flux (SCP + KryoFlux-style `.flux`)** | uploaded 2023 | IA item `amiga-original-xenon-2-powerplay` (metadata only) | **new** | real flux for the flux/retail path |
| **Amiga, Mirror Image budget** | 1991 | advertisement in Amiga Format 25 (August 1991), per a search summary of Hall of Light | **new (secondary)** | maybe imager "v4/v5"; a different build to check |
| **Amiga, Kixx budget** | 1993 | advertisement in Amiga Format 56 (February 1994), same source | **new (secondary)** | same |
| Amiga Format cover disk (Oct 1989, issue 3) | 1989 | secondary (search summary) | known (playable demo, IA `Xenon_2_Megablast_demo-playable_1989_Image_Works`) | already in our census |
| Amiga CDTV retail | 1992 | catalogued | known | the CDTV target (`cdtv-*.md`) |
| **CDTV review/promo CD-R** | c. 1992 | https://www.ami64.com/product-page/xenon-2-megablast-cdtv-promo-review-cd (fetched) | **new** | listed as five languages including a joke one ("Xenitese"); 12 tracks; "same game version" per the seller, unverified |
| CD32 | none | no CD32 release; the promo CD-R is said to run on a CD32/A1200 | n/a | none |
| **Atari ST, Image Works** | 1989 | https://www.atarimania.com/games/atari-st-games-xenon-ii-megablast-10904 (fetched): two single-sided disks, Pasti dumps, credits | **analysed** (`atari-st-analysis.md`: Copylock ST, payload not decrypted) | engine sibling; credits (Martin Day, Mark Coleman, Eric Matthews, David Whittaker, John Carpenter) |
| **Atari ST, ST Format preview** | Oct 1989 | https://demozoo.org/productions/123834/ (fetched): "Xenon 2: Megablast (Preview)", code Martin Day, graphics Mark Coleman, music David Whittaker; packed on demo CDs | **analysed**: a debug build with developer labels; 75-88% of it is in the Kixx resident engine | an earlier build. Download at fujiology.org (link only) |
| **Atari ST, MCM (Spain), Mirror Image (1991), Kixx (1993)** | 1989-93 | Atarimania Kixx page (fetched): re-release list; Kixx = one double-sided disk (Pasti and ST images) | **Kixx analysed** (RNC-packed, unprotected; engine unpacked and compared); **Mirror Image analysed** (the same Copylock ST as Image Works, re-keyed: not decoded; its second side is byte-identical to Image Works disk 2); MCM not held | `atari-st-analysis.md`, `atari-st-mirror-image.md` |
| **Atari ST, STe DMA-audio hack** | 2015 | Atarimania list; AtariAge thread (blocked); an 8bitchip HD patch page | **new (secondary)** | not a release; its audio source is unknown |
| PC / DOS (EGA, VGA, Tandy) | 1990 | IA `msdos_Xenon_2_-_Megablast_1990`, `xenon-2-megablast-1989`, KryoFlux streams `xenon2pc` | known (DOS image held) | flux set of the PC disks also exists |
| Bitmap Brothers Compilation (PC CD) | 1995 | IA `bitmap-brothers-compilation`, Redump 32647 | new, DOS only | no new build expected |
| Sega Master System | 1991, Virgin; Data Design Interactive | Wikipedia (fetched), Project Firestart | not examined | three levels instead of five; separate codebase |
| Sega Mega Drive | 1992, Virgin; Delvin Sorrell | Wikipedia (fetched) | known (6 ROMs, hacks) | no better assets (platform-images.md) |
| Game Boy (`Xenon II`) | 1992, Mindscape; Teeny Weeny Games | Wikipedia (fetched) | not examined | separate codebase |
| PC-98, X68000 | 1991, Epic/Sony | Wikipedia | known | no better assets |
| Acorn Archimedes **retail** | 1993, GamesWare; Eclipse | Arc Wiki https://arcwiki.org.uk/index.php/Xenon_2:_Megablast (fetched); stardot thread (fetched): original disks with copy protection | **new (full game)** | we hold only the playable demo; the retail is a larger data set |
| **Acorn User cover disc / CD 3 update** | Jan 1997 / Dec 1997 | Arc Wiki: updated by Stephen Harrison (VGA support, QTM music, protection removed); IA `cdrom-riscos-au-cd3` (`AUCD3.iso` 488,001,536 bytes) | **new** | a protection-free full Archimedes build, likely the easiest to obtain |
| Atari Jaguar | 2016 | Wikipedia (fetched), AtariAge store (fetched): a new conversion by "Cyrano Jones" with Mike Montgomery's blessing, new soundtrack by 505, from the Atari ST version | homebrew, not a release | derived work; a possible lead for how the ST data was reused |
| SNES | cancelled | Wikipedia "List of cancelled Super NES games" (search summary) | rumour (secondary) | nothing survives that we can find |
| Commodore 64 | never | https://www.gamesthatwerent.com/gtw64/xenon-2/ (fetched): the Bitmap Brothers discussed a conversion with a developer and "never heard from them again"; one submitted effort "is lost" | unreleased | no builds documented |
| ZX Spectrum | never | same page: David Spicer made a demo, rejected by Mirrorsoft | unreleased | no builds documented |
| Fan work: AGA 50 fps "Xenon 2 Recoded" (mcgeezer) | 2024- | https://www.indieretronews.com/2024/09/xenon-2-megablast-manic-retro-shoot-em.html (search summary only; the page was not fetched) | new, unreleased | engine rewrite from Devil's Temple code; says "first real Amiga version" |
| Fan work: Atari ST 50 fps cartridge (SidecarTridge `md-xenon-2`) | 2025 | https://github.com/sidecartridge/md-xenon-2 (fetched): GPLv3, rewritten in C for RP2040; needs the user's own Image Works disks; analysis tools in `tools/re/` | new | a parallel reverse-engineering effort on the ST build |
| Fan work: Go/Ebitengine recreation | | https://github.com/olivierh59500/xenon2 (fetched): needs a user ADF; no format docs | new | another parallel effort |

Note on dates. Wikipedia's page gives Master System 21 Nov 1991, Mega Drive Feb 1992, Game Boy and CDTV October 1992; a search
summary of the same page gave different months. Treat the day-level dates as unreliable; this survey does not depend on them.

Not found: any Amiga AGA/A1200 re-release, Steam, GOG, Evercade or Amiga Mini/THEA500 listing of Xenon 2 (a search returned
none). Amiga Forever's inclusion of the game was not checked.

## 3. Prototypes, previews and cover-disk builds

| Item | Evidence | Notes |
|---|---|---|
| Atari ST preview (ST Format, Oct 1989) | Demozoo (fetched), Atarimania | distinct production credited to Martin Day / Mark Coleman / David Whittaker; included on several demo CDs. A different build from the retail |
| Amiga Format 3 cover disk (Oct 1989) | search summary | the playable demo we already hold |
| Acorn User cover disc, October 1993 demo | Arc Wiki | the demo we hold (GamesWare `demo-playable`) |
| Unreleased: C64, Spectrum, SNES | section 2 | no surviving builds documented |
| Mega Drive / Master System prototypes | search found none | none known |

## 4. Existing reverse-engineering and reimplementation work

- SidecarTridge `md-xenon-2` (GitHub): rewrites the ST game in C and ships analysis tools; the author checked each routine against
  the original in Hatari. A useful second opinion for format knowledge.
- `olivierh59500/xenon2` (GitHub): Go recreation reading an ADF; no published format documentation.
- `smikes75/amiga-re-kit` (GitHub): a generic Amiga reverse-engineering kit (ADF/HUNK/packers, 68000 decoder and recompiler); not
  Xenon-specific.
- Nothing found that publishes a Xenon 2 disassembly or original source.

## 5. Development material and rights

**What exists, per public sources.**
- Credits (verified, multiple pages): design by The Bitmap Brothers (Eric Matthews credited for game design on the ST page), code
  by Martin Day of The Assembly Line, graphics Mark Coleman (DEGAS Elite plus animation routines, per a search summary), music
  by David Whittaker arranging Bomb the Bass's "Megablast (Hip Hop on Precinct 13)" (Tim Simenon), John Carpenter credited
  on the ST page. The CDTV version has a high-fidelity remix and voice acting credited to Richard Joseph (Wikipedia).
- The article https://www.generationamiga.com/2026/08/25/xenon-2-megablast-the-story-behind-its-famous-bomb-the-bass-soundtrack/
  (fetched) says Whittaker made a computer arrangement; it does **not** mention surviving masters, clearances or rights.
- Book: *The Bitmap Brothers: Universe* is described as combining first-hand interviews with Mike Montgomery and Dan Malone and
  "never-before-seen archive material" (https://www.thevideogamelibrary.org/book/the-bitmap-brothers-universe); useful for
  documents and art, not files. Not checked for Xenon 2 content.
- Interviews with Mike Montgomery exist (https://www.sega-16.com/2006/03/interview-mike-montgomery/,
  https://www.bit-tech.net/reviews/gaming/bitmap-brothers-mike-montgomery-interview/1/,
  https://www.retrovideogamer.co.uk/rvg-interviews-mike-montgomery/); **not read** for this note, so whether they say anything
  about surviving development files is unknown.
- Amiga Music Preservation (https://amp.dascene.net/detail.php?view=1655) lists David Whittaker (14 modules online) and
  Demozoo has a Xenon 2 music entry (https://demozoo.org/music/239781/); the AMP page itself was blocked, so what the Xenon 2
  entry is (a game rip, not original files) is not confirmed.
- No leak or release of source code, art files, tracker modules or Bomb the Bass stems was found anywhere in the search.

**Rights.**
- Rebellion Developments acquired "The Bitmap Brothers brand and portfolio" on 25 November 2019
  (https://rebellion.com/rebellion-acquires-the-bitmap-brothers-brand-and-portfolio/ , fetched); the release names Xenon,
  Z: Steel Soldiers, Speedball and The Chaos Engine, not Xenon 2, and says nothing about source code or archives.
- Mike Montgomery and John Phillips left the studio in 2004 (Wikipedia, The Bitmap Brothers); his 2016 "blessing" for the Jaguar
  conversion therefore predates Rebellion and was personal, not a licence from the current owner (our inference).
- The music rights are separate: Bomb the Bass, Rhythm King (named in the PC-98 executable per `platform-images.md`) and
  whoever now holds the Whittaker arrangement. Mirrorsoft, Image Works and Mindscape no longer exist as such; successors were
  not researched.
- Contacting any of these people or companies is the user's matter; this note only records who they are.

## 6. Recommended next acquisitions

**Status (2026-10-31): items 1-3 below were fetched and analysed; results in `acquisitions-flux-acorn-st.md`.** In short: the flux
item is the previously unseen PDos release (imager v5) and a real flux test; Acorn User CD 3 holds the complete '97 update
(ArcFS, with a ProTracker MEGABLAST module); the Atari ST disks are a separate packed port (no shared bytes with the Amiga or
CDTV builds on the disk; once unpacked, the ST engine resembles the CDTV code closely, see `atari-st-analysis.md`). The Internet Archive has no Xenon 2 ST dump; Atarimania has Image Works, Mirror Image, Kixx and ST Format
(issue 3).

Ranked by value and effort. The first three are covered by the downloads already approved for Xenon 2 material (D4); the others
need a decision.

1. **The Amiga retail flux on the Internet Archive** (`amiga-original-xenon-2-powerplay`, 36 MB SCP and 29 MB `.flux`):
   real flux of a retail original. It tests the SCP reader, the PLL on a real disk, and the retail key track, which no synthetic
   test can. Effort: minutes. Check first which disk it is.
2. **Acorn User CD 3 (`cdrom-riscos-au-cd3`, 488 MB ISO, or `acorn_user_cd3`, 429 MB zip)**: should contain the 1997
   protection-free full Archimedes release, to extend the Acorn survey (`platform-images.md` §7) from the demo to the full game.
   Effort: one large download plus decoding the Eclipse container; value is documentary (the art was identical in the demo).
3. **Atari ST Image Works dumps and the ST Format preview** (Atarimania Pasti zip; fujiology `XENON_II.ZIP`): a second build of
   the same engine and an earlier build. Effort: small downloads, a new container (Pasti STX), and a comparison of levels,
   tables and code with the Amiga build. Value: ground truth for what the ST shares with the Amiga.
4. **Amiga Mirror Image and Kixx budget disks** (TOSEC, WHDLoad imager versions "v4/v5"): likely the missing budget builds;
   may be in the TOSEC DAT already; check hashes against `docs/re-notes/sources.md`.
5. **CDTV promo CD-R**: check our disc's language table for a fifth ("Xenitese") entry; if the promo differs it is a
   different build (a seller's page only; do not buy on this evidence).
6. **The book *The Bitmap Brothers: Universe*** and the Montgomery interviews: for documentation and for names to contact.
   Contact with Rebellion or the former team about development files is the user's call.
7. **Master System and Game Boy ROMs**: separate codebases, no value for the Amiga recompiler; only for completeness.

Not recommended: the Jaguar homebrew and the fan remakes as sources of data (derived works, no new facts).
