// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// What the two window hosts of the UI (the launcher and the in-game menu) do with SDL beyond drawing: native file and folder dialogs whose
// answer arrives on another thread, opening a folder in the file manager, the Audio page's test tone, and starting the launcher again from
// the game's menu ("Back to launcher"). Needs SDL3.
#pragma once
#include <SDL3/SDL.h>

#include <mutex>
#include <string>
#include <vector>

#include "host_info.h"

namespace x2::ui {

// SDL's version and drivers, the platform and the compiler, for About and the diagnostics. `win` (may be null) gives the display's refresh rate.
HostInfo make_host_info(SDL_Window* win);

// "file:///path" for SDL_OpenURL (a path with a drive letter or UNC prefix gets the extra slash).
std::string file_url(const std::string& path);

// A native dialog's answer. SDL calls back from another thread on some platforms: the paths wait here for the UI thread, and a user event
// wakes a loop that sleeps in SDL_WaitEvent.
class FilePicker {
 public:
  void show_files(SDL_Window* win, bool many);
  void show_folder(SDL_Window* win);
  void show_save(SDL_Window* win, const char* default_name);
  // Moves the paths picked since the last call out.
  std::vector<std::string> take();
  // The callback itself, for the tests.
  void deliver(const char* const* list);

 private:
  static void SDLCALL on_picked(void* userdata, const char* const* list, int filter);
  std::mutex mu_;
  std::vector<std::string> paths_;
};

// The test tone of the Audio page on its own audio stream (the game's own device is paused while a menu is open). One tone at a time; a new
// one replaces the last. Opens SDL's audio subsystem on first use and closes it again when destroyed.
class TestTone {
 public:
  TestTone() = default;
  TestTone(const TestTone&) = delete;
  TestTone& operator=(const TestTone&) = delete;
  ~TestTone();
  // True when the tone was queued; `why` says what failed otherwise.
  bool play(int volume_percent, std::string* why = nullptr);
  // Call now and then: closes the stream once the tone has been heard.
  void service();
  bool playing() const { return stream_ != nullptr; }
  void stop() { close_stream(); }  // before the audio system is shut down

 private:
  void close_stream();
  SDL_AudioStream* stream_ = nullptr;
  Uint64 done_at_ = 0;
  bool inited_ = false;
};

// The arguments (program first) of the new process "Back to launcher" starts: the program, --gui, and --data-dir when this run had one.
std::vector<std::string> relaunch_args(const std::string& exe, const std::string& data_dir);
// Starts it detached. The caller exits right after. False when the process could not be started.
bool relaunch_launcher(const std::string& exe, const std::string& data_dir);

}  // namespace x2::ui
