// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "host_actions.h"

#include <cstdio>
#include <string>

#include "test_tone.h"

namespace x2::ui {

namespace {

const char* arch_name() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__arm__) || defined(_M_ARM)
  return "arm";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#else
  return "unknown cpu";
#endif
}

std::string compiler_name() {
#if defined(__clang__)
  return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
  return std::string("gcc ") + __VERSION__;
#elif defined(_MSC_VER)
  return "msvc " + std::to_string(_MSC_VER);
#else
  return "unknown compiler";
#endif
}

}  // namespace

HostInfo make_host_info(SDL_Window* win) {
  HostInfo h;
  const char* plat = SDL_GetPlatform();
  h.platform = std::string(plat ? plat : "unknown") + " " + arch_name();
  const int v = SDL_GetVersion();
  h.sdl_version = std::to_string(SDL_VERSIONNUM_MAJOR(v)) + "." + std::to_string(SDL_VERSIONNUM_MINOR(v)) + "." + std::to_string(SDL_VERSIONNUM_MICRO(v));
  if (const char* d = SDL_GetCurrentVideoDriver()) h.video_driver = d;
  if (const char* d = SDL_GetCurrentAudioDriver()) h.audio_driver = d;
  if (win) {
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win)))
      if (mode->refresh_rate > 0) h.display = std::to_string(static_cast<int>(mode->refresh_rate + 0.5f)) + " Hz";
  }
#ifdef NDEBUG
  h.build_id = compiler_name() + ", release";
#else
  h.build_id = compiler_name() + ", debug";
#endif
  return h;
}

std::string file_url(const std::string& path) {
  std::string u = "file://";
  if (path.empty() || path[0] != '/') u += '/';
  for (char ch : path) u += ch == '\\' ? '/' : ch;
  return u;
}

void FilePicker::deliver(const char* const* list) {
  if (list) {
    std::lock_guard<std::mutex> lock(mu_);
    for (; *list; ++list) paths_.emplace_back(*list);
  }
  SDL_Event wake{};
  wake.type = SDL_EVENT_USER;
  SDL_PushEvent(&wake);  // thread-safe: wakes a loop that waits for events
}

void SDLCALL FilePicker::on_picked(void* userdata, const char* const* list, int) { static_cast<FilePicker*>(userdata)->deliver(list); }

void FilePicker::show_files(SDL_Window* win, bool many) { SDL_ShowOpenFileDialog(on_picked, this, win, nullptr, 0, nullptr, many); }
void FilePicker::show_folder(SDL_Window* win) { SDL_ShowOpenFolderDialog(on_picked, this, win, nullptr, false); }
void FilePicker::show_save(SDL_Window* win, const char* default_name) { SDL_ShowSaveFileDialog(on_picked, this, win, nullptr, 0, default_name); }

std::vector<std::string> FilePicker::take() {
  std::vector<std::string> out;
  std::lock_guard<std::mutex> lock(mu_);
  out.swap(paths_);
  return out;
}

TestTone::~TestTone() {
  close_stream();
  if (inited_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void TestTone::close_stream() {
  if (stream_) SDL_DestroyAudioStream(stream_);
  stream_ = nullptr;
}

bool TestTone::play(int volume_percent, std::string* why) {
  auto fail = [&](const char* what) {
    if (why) *why = std::string(what) + ": " + SDL_GetError();
    return false;
  };
  close_stream();
  if (!SDL_WasInit(SDL_INIT_AUDIO)) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return fail("cannot start the audio system");
    inited_ = true;
  }
  constexpr int kRate = 48000, kMs = 450;
  const std::vector<int16_t> samples = make_test_chirp(kRate, kMs, volume_percent);
  SDL_AudioSpec spec{};
  spec.format = SDL_AUDIO_S16;
  spec.channels = 2;
  spec.freq = kRate;
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!stream_) return fail("cannot open the sound output");
  if (!SDL_PutAudioStreamData(stream_, samples.data(), static_cast<int>(samples.size() * sizeof(int16_t)))) {
    const bool ok = fail("cannot queue the tone");
    close_stream();
    return ok;
  }
  SDL_FlushAudioStream(stream_);
  SDL_ResumeAudioStreamDevice(stream_);
  done_at_ = SDL_GetTicks() + kMs + 250;
  return true;
}

void TestTone::service() {
  if (stream_ && SDL_GetTicks() >= done_at_) close_stream();
}

std::vector<std::string> relaunch_args(const std::string& exe, const std::string& data_dir) {
  std::vector<std::string> a{exe, "--gui"};
  if (!data_dir.empty()) {
    a.push_back("--data-dir");
    a.push_back(data_dir);
  }
  return a;
}

bool relaunch_launcher(const std::string& exe, const std::string& data_dir) {
  const std::vector<std::string> args = relaunch_args(exe, data_dir);
  std::vector<const char*> argv;
  for (const std::string& s : args) argv.push_back(s.c_str());
  argv.push_back(nullptr);
  SDL_Process* p = SDL_CreateProcess(argv.data(), false);
  if (!p) {
    std::fprintf(stderr, "x2recomp: cannot start the launcher: %s\n", SDL_GetError());
    return false;
  }
  SDL_DestroyProcess(p);  // detaches: it does not stop the new process
  return true;
}

}  // namespace x2::ui
