// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The program's own log: the last 500 lines in memory (the Advanced page shows them) and a rotating x2recomp.log in the data folder, which
// a double-clicked app needs because it has no console. Importer messages, pack reports, settings problems and the notices of the launcher
// and the in-game menu are written here. Thread-safe, never throws, and a failure to write the file is not an error for the program.
// Headless runs never open the file, so replays and tests leave the user's data folder alone.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "ui_widgets.h"

namespace x2::ui {

class LogBuffer {
 public:
  static constexpr size_t kMaxLines = 500;
  static constexpr size_t kMaxFileBytes = 256 * 1024;

  static LogBuffer& instance();

  // One line per '\n'. Empty text is ignored.
  void add(Severity severity, const std::string& text);
  std::vector<Message> lines() const;
  // Changes whenever a line is added: the page redraws its list when it differs from the last one it saw.
  uint64_t generation() const;

  // Starts writing to `path`. An existing file over kMaxFileBytes is first moved to `path`.1 (replacing an older one). False when the file
  // cannot be written; the buffer keeps working in memory.
  bool open_file(const std::string& path);
  void close_file();
  std::string file_path() const;

  void clear();  // the lines in memory (the file is kept)
  // The clock for the time stamps (seconds since 1970); tests give a fixed one.
  void set_clock(std::function<int64_t()> clock);

 private:
  void rotate_if_needed();
  mutable std::mutex mu_;
  std::vector<Message> lines_;
  uint64_t generation_ = 0;
  std::string path_;
  size_t written_ = 0;
  std::function<int64_t()> clock_;
};

inline void log_info(const std::string& t) { LogBuffer::instance().add(Severity::Info, t); }
inline void log_warn(const std::string& t) { LogBuffer::instance().add(Severity::Warn, t); }
inline void log_error(const std::string& t) { LogBuffer::instance().add(Severity::Error, t); }

}  // namespace x2::ui
