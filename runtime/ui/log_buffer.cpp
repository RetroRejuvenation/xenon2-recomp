// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "log_buffer.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <utility>

namespace x2::ui {

namespace fs = std::filesystem;

namespace {

const char* severity_word(Severity s) {
  switch (s) {
    case Severity::Ok: return "ok";
    case Severity::Warn: return "warning";
    case Severity::Error: return "error";
    default: return "info";
  }
}

std::string stamp(int64_t seconds) {
  const std::time_t t = static_cast<std::time_t>(seconds);
  std::tm tmv{};
#if defined(_WIN32)
  gmtime_s(&tmv, &t);
#else
  gmtime_r(&t, &tmv);
#endif
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  return buf;
}

}  // namespace

LogBuffer& LogBuffer::instance() {
  static LogBuffer log;
  return log;
}

void LogBuffer::set_clock(std::function<int64_t()> clock) {
  std::lock_guard<std::mutex> lock(mu_);
  clock_ = std::move(clock);
}

void LogBuffer::rotate_if_needed() {
  if (path_.empty() || written_ < kMaxFileBytes) return;
  std::error_code ec;
  fs::remove(path_ + ".1", ec);
  fs::rename(path_, path_ + ".1", ec);
  written_ = 0;
}

void LogBuffer::add(Severity severity, const std::string& text) {
  try {
    std::lock_guard<std::mutex> lock(mu_);
    const int64_t now = clock_ ? clock_() : static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                                                    std::chrono::system_clock::now().time_since_epoch()).count());
    size_t start = 0;
    while (start <= text.size()) {
      size_t end = text.find('\n', start);
      if (end == std::string::npos) end = text.size();
      std::string line = text.substr(start, end - start);
      while (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) {
        lines_.push_back({severity, line});
        if (lines_.size() > kMaxLines) lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(lines_.size() - kMaxLines));
        ++generation_;
        if (!path_.empty()) {
          rotate_if_needed();
          const std::string out = stamp(now) + " " + severity_word(severity) + ": " + line + "\n";
          std::ofstream f(path_, std::ios::app | std::ios::binary);
          if (f) {
            f << out;
            written_ += out.size();
          }
        }
      }
      if (end >= text.size()) break;
      start = end + 1;
    }
  } catch (...) {
    // logging must never take the program down
  }
}

std::vector<Message> LogBuffer::lines() const {
  std::lock_guard<std::mutex> lock(mu_);
  return lines_;
}

uint64_t LogBuffer::generation() const {
  std::lock_guard<std::mutex> lock(mu_);
  return generation_;
}

bool LogBuffer::open_file(const std::string& path) {
  std::lock_guard<std::mutex> lock(mu_);
  path_.clear();
  written_ = 0;
  if (path.empty()) return false;
  std::error_code ec;
  const auto size = fs::exists(path, ec) ? fs::file_size(path, ec) : 0;
  if (!ec && size > kMaxFileBytes) {
    fs::remove(path + ".1", ec);
    fs::rename(path, path + ".1", ec);
  }
  std::ofstream f(path, std::ios::app | std::ios::binary);
  if (!f) return false;
  path_ = path;
  std::error_code ec2;
  const auto now_size = fs::file_size(path, ec2);
  written_ = ec2 ? 0 : static_cast<size_t>(now_size);
  return true;
}

void LogBuffer::close_file() {
  std::lock_guard<std::mutex> lock(mu_);
  path_.clear();
  written_ = 0;
}

std::string LogBuffer::file_path() const {
  std::lock_guard<std::mutex> lock(mu_);
  return path_;
}

void LogBuffer::clear() {
  std::lock_guard<std::mutex> lock(mu_);
  lines_.clear();
  ++generation_;
}

}  // namespace x2::ui
