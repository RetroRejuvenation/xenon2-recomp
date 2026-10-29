// x2recomp reference harness (Phase 4). Included at the end of amiberry_gfx.cpp so it can use
// save_png() and amiga_surface. Everything is driven by environment variables and is inert when
// X2REF_DIR is unset:
//   X2REF_DIR    output directory (frame log, PNGs, WAV, RAM dumps)
//   X2REF_SHOTS  frames to save as PNG, e.g. "100,200-260,1000-2000/50" (/n = every n-th)
//   X2REF_RAM    frames at which to dump the 512K chip RAM, same syntax
//   X2REF_INPUT  script file, lines "<frame> <event> <0|1>" with event one of
//                JOY1_FIRE (port 1 fire = mouse left button), JOY2_FIRE, JOY2_LEFT, JOY2_RIGHT,
//                JOY2_UP, JOY2_DOWN, KEY<hex raw code>, or MOUSE1_VERT/MOUSE1_HORIZ with a signed
//                relative count instead of 0|1 (port 1 mouse), or SWAP_DF0 1 (insert the DF1 image
//                into DF0, as a user swapping to the level disc would)
//                Game-time lines (for scripts converted from x2recomp replays by tools/compare/to_harness.py):
//                "BASE <vsync>" starts game time G at that vsync (G = vsync - BASE - paused frames), then
//                "G<frame> <event> <state>"                   an input event at game frame G
//                "G<frame> POKE <addr> <value> <size> <n>"    COVERAGE ONLY: write chip RAM (hex addr/value,
//                                                              size 1/2/4 bytes) at the start of game frames G..G+n-1
//                "G<frame> POKEOBJ <handler> <off> <value> <size> <n>"  the same, at offset <off> in every object
//                                                              of the hittable-object list whose hit handler is
//                                                              <handler> (as x2recomp's x2::apply_poke)
//                "G<frame> WAITLOAD <q>"                      hold game time at G until a disc load (VBL vector $6C
//                                                              = $2098) has been seen and has then been over for q
//                                                              vsyncs; game time resumes at G+q-1. x2recomp loads
//                                                              instantly, so this re-aligns the two timelines.
//                "G<frame> WAITSTEP <q> <g>"                  as WAITLOAD, then hold on until a vsync at which the
//                                                              game's last-swap VBL count $404 has changed since the
//                                                              previous vsync (a logic step has just begun); that
//                                                              vsync is game frame <g> (x2recomp's first step start
//                                                              after the same load), so both runs resume in step
//                "G<frame> SHOT" / "G<frame> RAM"             a PNG / chip RAM dump of the vsync at which game time
//                                                              reaches <frame> (named by vsync, as X2REF_SHOTS/RAM)
//   X2REF_WATCH  chip RAM word addresses (hex) logged every frame, default 404,41C,410,406,40A,700E
//   X2REF_QUIT   frame at which to quit
//   X2REF_WAV    1 = tap the Paula mix into paula.wav (sample positions per frame go to frames.log)
//   X2REF_IDLE   PC ranges "A-B,..." (hex, B exclusive): frames.log column "idle" counts the hsyncs of each frame
//                at which the CPU PC was in one of them (e.g. the main loop's swap wait $2034-$2042); column
//                "blt" counts the hsyncs at which the blitter was busy
//   X2REF_PCTRACE vsync range "A-B": pctrace.log gets "vsync vpos pc" at every hsync (end of line vpos)
//   X2REF_PCLOG  "A-B:pc,pc,..." (vsync range, hex PCs): pclog.log gets "vsync vpos hpos cycles pc" whenever the
//                cycle-exact CPU starts an instruction at one of the PCs (cycles: CPU cycles since reset)
//   X2REF_INSNLOG vsync ranges "A-B,C-D": insn.log gets "vsync vpos hpos cycles pc opcode blitbusy" for every
//                instruction the cycle-exact CPU starts in them (per-opcode timing calibration)
//   X2REF_BLITLOG vsync range "A-B": blits.log gets "vsync vpos hpos S w h bltcon0 bltcon1" at each blit start
//                and "vsync vpos hpos E" when it finishes (BBUSY clear, blitter interrupt)
// Frame numbers are Amiberry's vsync_counter (reset to 0 at hard reset).
#include "inputdevice.h"
#include "disk.h"
#include "blitter.h"
#include <vector>
#include <set>

extern uae_u32 vsync_counter;

bool x2ref_pclog_on = false; // newcpu.cpp tests it before calling x2ref_insn()

namespace {
struct X2Event { uae_u32 frame; int ev; int key; int state; };
// Game-time actions: kind 0 input (as X2Event), 1 POKE, 2 POKEOBJ, 3 WAITLOAD, 4 SHOT, 5 RAM, 6 WAITSTEP
struct X2GameEv { uae_u32 frame; int kind; X2Event in; uae_u32 addr, value, handler, count; int size; };
struct X2Poke { uae_u32 from, to, addr, value, handler; int size; };

bool x2_on = false;
std::string x2_dir;
std::set<uae_u32> x2_shots, x2_ram;
std::vector<X2Event> x2_events;
size_t x2_next_event = 0;
std::vector<X2GameEv> x2_game;
std::vector<X2Poke> x2_pokes;
size_t x2_next_game = 0;
long x2_base = -1;           // vsync at which game time 0 starts (BASE)
uae_u32 x2_paused = 0;       // vsyncs game time was held by WAITLOAD
int x2_wait_q = 0;           // >0 while a WAITLOAD is pending
bool x2_wait_seen = false;   // a load was seen since the WAITLOAD started
int x2_wait_idle = 0;        // consecutive vsyncs without a load since then
long x2_wait_step = -1;      // WAITSTEP: game frame to resume at, once a logic step begins
int x2_prev404 = -1;         // $404 at the previous vsync (WAITSTEP)
std::vector<uae_u32> x2_watch = { 0x404, 0x41c, 0x410, 0x406, 0x40a, 0x700e };
long x2_quit = -1;
FILE* x2_log = nullptr;
FILE* x2_wav = nullptr;
uae_u64 x2_wav_bytes = 0;
bool x2_init_done = false;
std::vector<std::pair<uae_u32, uae_u32>> x2_idle;  // X2REF_IDLE
uae_u32 x2_idle_lines = 0, x2_blt_lines = 0;       // per frame
uae_u32 x2_blitlog_from = 1, x2_blitlog_to = 0;     // X2REF_BLITLOG
FILE* x2_blitlog = nullptr;
std::vector<std::pair<uae_u32, uae_u32>> x2_insnlog; // X2REF_INSNLOG
FILE* x2_insnlog_f = nullptr;
uae_u32 x2_pclog_from = 1, x2_pclog_to = 0;         // X2REF_PCLOG
std::set<uae_u32> x2_pclog_pcs;
FILE* x2_pclog = nullptr;
uae_u32 x2_pctrace_from = 1, x2_pctrace_to = 0;     // X2REF_PCTRACE
FILE* x2_pctrace = nullptr;

void x2_parse_set(const char* s, std::set<uae_u32>& out)
{
	while (s && *s) {
		char* end;
		const unsigned long a = strtoul(s, &end, 10);
		unsigned long b = a, step = 1;
		if (*end == '-')
			b = strtoul(end + 1, &end, 10);
		if (*end == '/')
			step = strtoul(end + 1, &end, 10);
		for (unsigned long f = a; f <= b; f += step ? step : 1)
			out.insert(static_cast<uae_u32>(f));
		s = *end ? end + 1 : end;
	}
}

void x2_wav_header(FILE* f, uae_u64 data)
{
	auto le32 = [f](uae_u32 v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); fputc(v >> 16 & 255, f); fputc(v >> 24, f); };
	auto le16 = [f](int v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); };
	fseek(f, 0, SEEK_SET);
	fwrite("RIFF", 1, 4, f); le32(static_cast<uae_u32>(36 + data)); fwrite("WAVEfmt ", 1, 8, f);
	le32(16); le16(1); le16(2); le32(currprefs.sound_freq); le32(currprefs.sound_freq * 4); le16(4); le16(16);
	fwrite("data", 1, 4, f); le32(static_cast<uae_u32>(data));
	fseek(f, 0, SEEK_END);
}

void x2_init()
{
	x2_init_done = true;
	const char* dir = getenv("X2REF_DIR");
	if (!dir || !*dir)
		return;
	x2_on = true;
	x2_dir = dir;
	x2_parse_set(getenv("X2REF_SHOTS"), x2_shots);
	x2_parse_set(getenv("X2REF_RAM"), x2_ram);
	if (const char* q = getenv("X2REF_QUIT"))
		x2_quit = atol(q);
	if (const char* w = getenv("X2REF_WATCH")) {
		x2_watch.clear();
		for (const char* s = w; *s;) {
			char* end;
			x2_watch.push_back(strtoul(s, &end, 16));
			s = *end ? end + 1 : end;
		}
	}
	if (const char* in = getenv("X2REF_INPUT")) {
		if (FILE* f = fopen(in, "r")) {
			char line[256], name[64];
			unsigned long fr;
			int st;
			while (fgets(line, sizeof line, f)) {
				if (line[0] == '#')
					continue;
				unsigned long base;
				if (sscanf(line, "BASE %lu", &base) == 1) {
					x2_base = static_cast<long>(base);
					continue;
				}
				const bool game = line[0] == 'G';
				char* rest = line + (game ? 1 : 0);
				if (game) {
					unsigned long a, v, h, n;
					int sz, q;
					X2GameEv g{};
					if (sscanf(rest, "%lu POKE %lx %lx %d %lu", &fr, &a, &v, &sz, &n) == 5) {
						g = { static_cast<uae_u32>(fr), 1, {}, static_cast<uae_u32>(a), static_cast<uae_u32>(v), 0, static_cast<uae_u32>(n), sz };
						x2_game.push_back(g);
						continue;
					}
					if (sscanf(rest, "%lu POKEOBJ %lx %lx %lx %d %lu", &fr, &h, &a, &v, &sz, &n) == 6) {
						g = { static_cast<uae_u32>(fr), 2, {}, static_cast<uae_u32>(a), static_cast<uae_u32>(v), static_cast<uae_u32>(h), static_cast<uae_u32>(n), sz };
						x2_game.push_back(g);
						continue;
					}
					char what[8];
					if (sscanf(rest, "%lu %7s", &fr, what) == 2 && (!strcmp(what, "SHOT") || !strcmp(what, "RAM"))) {
						g = { static_cast<uae_u32>(fr), what[0] == 'S' ? 4 : 5, {}, 0, 0, 0, 0, 0 };
						x2_game.push_back(g);
						continue;
					}
					unsigned long gb;
					if (sscanf(rest, "%lu WAITSTEP %d %lu", &fr, &q, &gb) == 3) {
						g = { static_cast<uae_u32>(fr), 6, {}, static_cast<uae_u32>(gb), 0, 0, static_cast<uae_u32>(q > 0 ? q : 1), 0 };
						x2_game.push_back(g);
						continue;
					}
					if (sscanf(rest, "%lu WAITLOAD %d", &fr, &q) == 2) {
						g = { static_cast<uae_u32>(fr), 3, {}, 0, 0, 0, static_cast<uae_u32>(q > 0 ? q : 1), 0 };
						x2_game.push_back(g);
						continue;
					}
				}
				if (sscanf(rest, "%lu %63s %d", &fr, name, &st) != 3)
					continue;
				X2Event e{ static_cast<uae_u32>(fr), 0, -1, st };
				const std::string n = name;
				if (n == "JOY1_FIRE") e.ev = INPUTEVENT_JOY1_FIRE_BUTTON;
				else if (n == "JOY2_FIRE") e.ev = INPUTEVENT_JOY2_FIRE_BUTTON;
				else if (n == "JOY2_LEFT") e.ev = INPUTEVENT_JOY2_LEFT;
				else if (n == "JOY2_RIGHT") e.ev = INPUTEVENT_JOY2_RIGHT;
				else if (n == "JOY2_UP") e.ev = INPUTEVENT_JOY2_UP;
				else if (n == "JOY2_DOWN") e.ev = INPUTEVENT_JOY2_DOWN;
				else if (n == "MOUSE1_VERT") { e.ev = INPUTEVENT_MOUSE1_VERT; e.key = -2; }
				else if (n == "MOUSE1_HORIZ") { e.ev = INPUTEVENT_MOUSE1_HORIZ; e.key = -2; }
				else if (n == "SWAP_DF0") e.key = -3;
				else if (n.rfind("KEY", 0) == 0) e.key = static_cast<int>(strtol(n.c_str() + 3, nullptr, 16));
				else continue;
				if (game)
					x2_game.push_back({ e.frame, 0, e, 0, 0, 0, 0, 0 });
				else
					x2_events.push_back(e);
			}
			fclose(f);
		}
	}
	if (const char* r = getenv("X2REF_IDLE")) {
		for (const char* s = r; *s;) {
			char* end;
			const uae_u32 a = strtoul(s, &end, 16);
			uae_u32 b = a + 2;
			if (*end == '-')
				b = strtoul(end + 1, &end, 16);
			x2_idle.push_back({ a, b });
			s = *end ? end + 1 : end;
		}
	}
	if (const char* r = getenv("X2REF_BLITLOG")) {
		char* end;
		x2_blitlog_from = strtoul(r, &end, 10);
		x2_blitlog_to = *end == '-' ? strtoul(end + 1, nullptr, 10) : x2_blitlog_from;
		x2_blitlog = fopen((x2_dir + "/blits.log").c_str(), "w");
	}
	if (const char* r = getenv("X2REF_PCLOG")) {
		char* end;
		x2_pclog_from = strtoul(r, &end, 10);
		x2_pclog_to = *end == '-' ? strtoul(end + 1, &end, 10) : x2_pclog_from;
		while (*end == ':' || *end == ',') {
			const char* s = end + 1;
			x2_pclog_pcs.insert(strtoul(s, &end, 16));
			if (end == s)
				break;
		}
		x2_pclog = fopen((x2_dir + "/pclog.log").c_str(), "w");
		x2ref_pclog_on = x2_pclog != nullptr;
	}
	if (const char* r = getenv("X2REF_INSNLOG")) {
		for (const char* s = r; *s;) {
			char* end;
			const uae_u32 a = strtoul(s, &end, 10);
			uae_u32 b = a;
			if (*end == '-')
				b = strtoul(end + 1, &end, 10);
			x2_insnlog.push_back({ a, b });
			s = *end ? end + 1 : end;
		}
		x2_insnlog_f = fopen((x2_dir + "/insn.log").c_str(), "w");
		x2ref_pclog_on = x2_insnlog_f != nullptr;
	}
	if (const char* r = getenv("X2REF_PCTRACE")) {
		char* end;
		x2_pctrace_from = strtoul(r, &end, 10);
		x2_pctrace_to = *end == '-' ? strtoul(end + 1, nullptr, 10) : x2_pctrace_from;
		x2_pctrace = fopen((x2_dir + "/pctrace.log").c_str(), "w");
	}
	x2_log = fopen((x2_dir + "/frames.log").c_str(), "w");
	if (x2_log) {
		fprintf(x2_log, "# frame cop1lc wav_samples");
		for (auto a : x2_watch)
			fprintf(x2_log, " w%04X", a);
		fprintf(x2_log, " idle blt game_frame\n");
	}
	const char* wav = getenv("X2REF_WAV");
	if (wav && *wav == '1') {
		x2_wav = fopen((x2_dir + "/paula.wav").c_str(), "wb");
		if (x2_wav)
			x2_wav_header(x2_wav, 0);
	}
}
}

namespace {
void x2_send(const X2Event& e, uae_u32 fr)
{
	if (e.key >= 0)
		inputdevice_do_keyboard(e.key, e.state);
	else if (e.key == -3)
		disk_insert(0, currprefs.floppyslots[1].df);
	else if (e.key == -2)
		send_input_event(e.ev, e.state, 0, 0); // max 0: relative mouse count
	else
		send_input_event(e.ev, e.state, 1, 0);
	write_log(_T("X2REF: frame %u input %d key %d state %d\n"), fr, e.ev, e.key, e.state);
}

// x2recomp's x2::apply_poke: a plain chip write, or one into every object on the hittable-object list (head
// $B4C, next at +$12, a type word of 0 ends it, at most 1024 nodes) whose hit handler (+$A) matches.
void x2_apply_poke(uae_u8* chip, uae_u32 mask, const X2Poke& p)
{
	auto put = [&](uae_u32 a) {
		for (int i = 0; i < p.size; ++i)
			chip[(a + i) & mask] = static_cast<uae_u8>(p.value >> (8 * (p.size - 1 - i)));
	};
	auto rd16 = [&](uae_u32 a) { return static_cast<uae_u32>(chip[a & mask] << 8 | chip[(a + 1) & mask]); };
	auto rd32 = [&](uae_u32 a) { return rd16(a) << 16 | rd16(a + 2); };
	if (!p.handler) {
		put(p.addr);
		return;
	}
	uae_u32 a = rd32(0xB4C) & mask;
	for (int n = 0; n < 1024 && a && rd16(a) != 0; ++n) {
		if (rd32(a + 0xA) == p.handler)
			put(a + p.addr);
		a = rd32(a + 0x12) & mask;
	}
}

// Advance game time by one vsync: WAITLOAD, game-time events, then the active pokes. Returns game time or -1.
long x2_game_step(uae_u32 fr)
{
	if (x2_base < 0 || static_cast<long>(fr) < x2_base)
		return -1;
	uae_u8* chip = chipmem_bank.baseaddr;
	const uae_u32 mask = chipmem_bank.allocated_size - 1;
	if (x2_wait_q > 0) {
		const bool loading = chip && (chip[0x6e] << 8 | chip[0x6f]) == 0x2098 && !(chip[0x6c] | chip[0x6d]);
		if (loading) {
			x2_wait_seen = true;
			x2_wait_idle = 0;
		} else if (x2_wait_seen) {
			++x2_wait_idle;
		}
		const int w404 = chip ? (chip[0x404] << 8 | chip[0x405]) : 0;
		const bool step_begun = x2_prev404 >= 0 && w404 != x2_prev404;
		x2_prev404 = w404;
		if (x2_wait_seen && x2_wait_idle >= x2_wait_q && (x2_wait_step < 0 || step_begun)) {
			// resume: this vsync is game frame WAITLOAD + q - 1, or the WAITSTEP frame
			const X2GameEv& w = x2_game[x2_next_game - 1];
			const long resume = x2_wait_step >= 0 ? x2_wait_step : static_cast<long>(w.frame + x2_wait_q - 1);
			x2_paused = fr - x2_base - resume;
			write_log(_T("X2REF: frame %u WAITLOAD %u resumed at game frame %ld, paused %u\n"), fr, w.frame, resume, x2_paused);
			x2_wait_q = 0;
			x2_wait_step = -1;
		} else {
			++x2_paused;
		}
	}
	const long gt = static_cast<long>(fr) - x2_base - static_cast<long>(x2_paused);
	while (!x2_wait_q && x2_next_game < x2_game.size() && static_cast<long>(x2_game[x2_next_game].frame) <= gt) {
		const X2GameEv& g = x2_game[x2_next_game++];
		if (g.kind == 0)
			x2_send(g.in, fr);
		else if (g.kind == 3 || g.kind == 6) {
			x2_wait_q = static_cast<int>(g.count);
			x2_wait_seen = false;
			x2_wait_idle = 0;
			x2_wait_step = g.kind == 6 ? static_cast<long>(g.addr) : -1;
			x2_prev404 = -1;
		} else if (g.kind == 4)
			x2_shots.insert(fr);
		else if (g.kind == 5)
			x2_ram.insert(fr);
		else
			x2_pokes.push_back({ g.frame, g.frame + g.count - 1, g.addr, g.value, g.kind == 2 ? g.handler : 0, g.size });
	}
	if (chip)
		for (const X2Poke& p : x2_pokes)
			if (gt >= static_cast<long>(p.from) && gt <= static_cast<long>(p.to))
				x2_apply_poke(chip, mask, p);
	return gt;
}
}

// Called from finish_sound_buffer() with the finished host-rate Paula buffer (stereo s16).
void x2ref_audio(const uae_u8* buf, int bytes)
{
	if (!x2_init_done)
		x2_init();
	if (!x2_wav || bytes <= 0)
		return;
	fwrite(buf, 1, bytes, x2_wav);
	x2_wav_bytes += bytes;
}

// Called once per emulated frame, right after vsync_counter is incremented.
void x2ref_vsync()
{
	if (!x2_init_done)
		x2_init();
	if (!x2_on)
		return;
	const uae_u32 fr = vsync_counter;
	while (x2_next_event < x2_events.size() && x2_events[x2_next_event].frame <= fr) {
		x2_send(x2_events[x2_next_event++], fr);
	}
	const long gt = x2_game_step(fr);
	if (x2_log) {
		const uae_u8* chip = chipmem_bank.baseaddr;
		const uae_u64 pending = static_cast<uae_u64>(reinterpret_cast<uae_u8*>(paula_sndbufpt) - reinterpret_cast<uae_u8*>(paula_sndbuffer));
		fprintf(x2_log, "%u %06X %llu", fr, get_copper_address(1), static_cast<unsigned long long>((x2_wav_bytes + pending) / 4));
		for (auto a : x2_watch)
			fprintf(x2_log, " %04X", chip ? (chip[a] << 8 | chip[a + 1]) : 0);
		fprintf(x2_log, " %u %u %ld\n", x2_idle_lines, x2_blt_lines, gt);
	}
	x2_idle_lines = x2_blt_lines = 0;
	if (x2_ram.count(fr) && chipmem_bank.baseaddr) {
		char name[64];
		snprintf(name, sizeof name, "/chip_%06u.bin", fr);
		if (FILE* f = fopen((x2_dir + name).c_str(), "wb")) {
			fwrite(chipmem_bank.baseaddr, 1, chipmem_bank.allocated_size, f);
			fclose(f);
		}
	}
	if (x2_quit >= 0 && fr >= static_cast<uae_u32>(x2_quit)) {
		x2_quit = -1;
		if (x2_wav) {
			x2_wav_header(x2_wav, x2_wav_bytes);
			fclose(x2_wav);
			x2_wav = nullptr;
		}
		if (x2_log) {
			fclose(x2_log);
			x2_log = nullptr;
		}
		if (x2_blitlog) {
			fclose(x2_blitlog);
			x2_blitlog = nullptr;
		}
		if (x2_pctrace) {
			fclose(x2_pctrace);
			x2_pctrace = nullptr;
		}
		if (x2_pclog) {
			fclose(x2_pclog);
			x2_pclog = nullptr;
		}
		if (x2_insnlog_f) {
			fclose(x2_insnlog_f);
			x2_insnlog_f = nullptr;
		}
		x2ref_pclog_on = false;
		uae_quit();
	}
}

// Called from the cycle-exact CPU loop (newcpu.cpp m68k_run_1_ce) before each instruction while X2REF_PCLOG is on.
void x2ref_insn(uaecptr pc)
{
	if (x2_insnlog_f) {
		for (const auto& r : x2_insnlog)
			if (vsync_counter >= r.first && vsync_counter <= r.second) {
				fprintf(x2_insnlog_f, "%u %d %d %lld %06X %04X %d\n", vsync_counter, vpos, current_hpos(),
					static_cast<long long>(get_cycles() / (CYCLE_UNIT / 2)), pc & 0xffffff, regs.opcode,
					(blt_info.blit_main || blt_info.blit_finald) ? 1 : 0);
				break;
			}
	}
	if (!x2_pclog || vsync_counter < x2_pclog_from || vsync_counter > x2_pclog_to || !x2_pclog_pcs.count(pc & 0xffffff))
		return;
	fprintf(x2_pclog, "%u %d %d %lld %06X\n", vsync_counter, vpos, current_hpos(),
		static_cast<long long>(get_cycles() / (CYCLE_UNIT / 2)), pc & 0xffffff);
}

// Called at every hsync, before vpos moves on to the next line.
void x2ref_hsync()
{
	if (!x2_on)
		return;
	if (!x2_idle.empty()) {
		const uae_u32 pc = m68k_getpc() & 0xffffff;
		for (const auto& r : x2_idle)
			if (pc >= r.first && pc < r.second) {
				++x2_idle_lines;
				break;
			}
	}
	if (blt_info.blit_main || blt_info.blit_finald)
		++x2_blt_lines;
	if (x2_pctrace && vsync_counter >= x2_pctrace_from && vsync_counter <= x2_pctrace_to)
		fprintf(x2_pctrace, "%u %d %06X\n", vsync_counter, vpos, m68k_getpc() & 0xffffff);
}

// Called at each blit start (what 0, from do_blitter) and when BBUSY clears (what 1, blitter_interrupt).
void x2ref_blit(int what)
{
	if (!x2_blitlog || vsync_counter < x2_blitlog_from || vsync_counter > x2_blitlog_to)
		return;
	if (what == 0)
		fprintf(x2_blitlog, "%u %d %d S %d %d %04X %04X\n", vsync_counter, vpos, current_hpos(), blt_info.hblitsize,
			blt_info.vblitsize, bltcon0, bltcon1);
	else
		fprintf(x2_blitlog, "%u %d %d E\n", vsync_counter, vpos, current_hpos());
}

// Called after a frame has been rendered into amiga_surface.
void x2ref_frame_rendered()
{
	if (!x2_on || !x2_shots.count(vsync_counter) || !create_screenshot())
		return;
	char name[64];
	snprintf(name, sizeof name, "/frame_%06u.png", vsync_counter);
	save_thumb(x2_dir + name);
}
