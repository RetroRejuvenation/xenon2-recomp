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
//   X2REF_WATCH  chip RAM word addresses (hex) logged every frame, default 404,41C,410,406,40A,700E
//   X2REF_QUIT   frame at which to quit
//   X2REF_WAV    1 = tap the Paula mix into paula.wav (sample positions per frame go to frames.log)
// Frame numbers are Amiberry's vsync_counter (reset to 0 at hard reset).
#include "inputdevice.h"
#include "disk.h"
#include <vector>
#include <set>

extern uae_u32 vsync_counter;

namespace {
struct X2Event { uae_u32 frame; int ev; int key; int state; };

bool x2_on = false;
std::string x2_dir;
std::set<uae_u32> x2_shots, x2_ram;
std::vector<X2Event> x2_events;
size_t x2_next_event = 0;
std::vector<uae_u32> x2_watch = { 0x404, 0x41c, 0x410, 0x406, 0x40a, 0x700e };
long x2_quit = -1;
FILE* x2_log = nullptr;
FILE* x2_wav = nullptr;
uae_u64 x2_wav_bytes = 0;
bool x2_init_done = false;

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
				if (line[0] == '#' || sscanf(line, "%lu %63s %d", &fr, name, &st) != 3)
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
				x2_events.push_back(e);
			}
			fclose(f);
		}
	}
	x2_log = fopen((x2_dir + "/frames.log").c_str(), "w");
	if (x2_log) {
		fprintf(x2_log, "# frame cop1lc wav_samples");
		for (auto a : x2_watch)
			fprintf(x2_log, " w%04X", a);
		fprintf(x2_log, "\n");
	}
	const char* wav = getenv("X2REF_WAV");
	if (wav && *wav == '1') {
		x2_wav = fopen((x2_dir + "/paula.wav").c_str(), "wb");
		if (x2_wav)
			x2_wav_header(x2_wav, 0);
	}
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
		const X2Event& e = x2_events[x2_next_event++];
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
	if (x2_log) {
		const uae_u8* chip = chipmem_bank.baseaddr;
		const uae_u64 pending = static_cast<uae_u64>(reinterpret_cast<uae_u8*>(paula_sndbufpt) - reinterpret_cast<uae_u8*>(paula_sndbuffer));
		fprintf(x2_log, "%u %06X %llu", fr, get_copper_address(1), static_cast<unsigned long long>((x2_wav_bytes + pending) / 4));
		for (auto a : x2_watch)
			fprintf(x2_log, " %04X", chip ? (chip[a] << 8 | chip[a + 1]) : 0);
		fprintf(x2_log, "\n");
	}
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
		uae_quit();
	}
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
