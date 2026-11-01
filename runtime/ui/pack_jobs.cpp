// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "pack_jobs.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace x2::ui {

namespace fs = std::filesystem;

// ---- the dump: arguments, folders ----

std::vector<std::string> dump_command_args(const DumpRequest& r, const std::string& into_dir) {
  std::vector<std::string> a = {"--headless", "--report-every", "100", "--game", r.game};
  if (!r.assets_dir.empty()) {
    a.push_back("--assets");
    a.push_back(r.assets_dir);
  }
  if (!r.data_dir.empty()) {
    a.push_back("--data-dir");
    a.push_back(r.data_dir);
  }
  a.push_back("--frames");
  a.push_back(std::to_string(r.frames));
  a.push_back("--dump-assets");
  a.push_back(into_dir);
  if (r.audio && r.game != "st") {
    a.push_back("--dump-audio");
    a.push_back(into_dir);
  }
  return a;
}

namespace {

std::string dump_name(const std::string& game, int n) { return n <= 1 ? game : game + "-" + std::to_string(n); }

bool exists_path(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}

// "[frames 0-99] pc=..." -> 99, or -1.
long frame_of_report(const std::string& line) {
  static const char kPrefix[] = "[frames ";
  if (line.compare(0, sizeof kPrefix - 1, kPrefix) != 0) return -1;
  const size_t dash = line.find('-', sizeof kPrefix - 1);
  if (dash == std::string::npos) return -1;
  char* end = nullptr;
  const long v = std::strtol(line.c_str() + dash + 1, &end, 10);
  return end == line.c_str() + dash + 1 ? -1 : v;
}

bool starts_with(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

#ifdef _WIN32
std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

// One argument as the C runtime's command-line parser reads it back.
std::wstring quote_arg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
  std::wstring r = L"\"";
  size_t backslashes = 0;
  for (const wchar_t c : a) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') r.append(backslashes * 2 + 1, L'\\');
    else r.append(backslashes, L'\\');
    backslashes = 0;
    r += c;
  }
  r.append(backslashes * 2, L'\\');
  r += L'"';
  return r;
}
#endif

// Runs `exe args`, calls on_line for every output line (standard output and error together), and returns when it ends or when `cancel`
// turns true (the process is then killed). False with `err` when it could not be started.
bool run_and_read(const std::string& exe, const std::vector<std::string>& args, const std::function<void(const std::string&)>& on_line,
                  const std::atomic<bool>& cancel, int& exit_code, std::string& err) {
  std::string pending;
  auto feed = [&](const char* data, size_t n) {
    pending.append(data, n);
    size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, nl);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      pending.erase(0, nl + 1);
      on_line(line);
    }
  };
#ifdef _WIN32
  SECURITY_ATTRIBUTES sa;
  std::memset(&sa, 0, sizeof sa);
  sa.nLength = sizeof sa;
  sa.bInheritHandle = TRUE;
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) {
    err = "Could not create a pipe to read the run.";
    return false;
  }
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  std::wstring cmd = quote_arg(widen(exe));
  for (const std::string& a : args) cmd += L" " + quote_arg(widen(a));
  STARTUPINFOW si;
  std::memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = INVALID_HANDLE_VALUE;
  si.hStdOutput = wr;
  si.hStdError = wr;
  PROCESS_INFORMATION pi;
  std::memset(&pi, 0, sizeof pi);
  const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(wr);
  if (!ok) {
    CloseHandle(rd);
    err = "Could not start " + exe + ".";
    return false;
  }
  char buf[4096];
  bool killed = false;
  for (;;) {
    DWORD avail = 0;
    if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
      DWORD got = 0;
      if (ReadFile(rd, buf, static_cast<DWORD>(std::min<size_t>(sizeof buf, avail)), &got, nullptr) && got > 0) feed(buf, got);
      continue;
    }
    if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
      DWORD got = 0;
      while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0 &&
             ReadFile(rd, buf, static_cast<DWORD>(std::min<size_t>(sizeof buf, avail)), &got, nullptr) && got > 0)
        feed(buf, got);
      break;
    }
    if (cancel.load() && !killed) {
      TerminateProcess(pi.hProcess, 1);
      killed = true;
    }
  }
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  exit_code = static_cast<int>(code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  CloseHandle(rd);
#else
  int fds[2];
  if (pipe(fds) != 0) {
    err = "Could not create a pipe to read the run.";
    return false;
  }
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
  posix_spawn_file_actions_addclose(&fa, fds[0]);
  posix_spawn_file_actions_addclose(&fa, fds[1]);
  std::vector<std::string> store;
  store.push_back(exe);
  store.insert(store.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (std::string& s : store) argv.push_back(s.data());
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  close(fds[1]);
  if (rc != 0) {
    close(fds[0]);
    err = "Could not start " + exe + ".";
    return false;
  }
  char buf[4096];
  bool killed = false;
  for (;;) {
    pollfd p;
    p.fd = fds[0];
    p.events = POLLIN;
    p.revents = 0;
    const int n = poll(&p, 1, 50);
    if (n > 0) {
      const ssize_t got = read(fds[0], buf, sizeof buf);
      if (got > 0) feed(buf, static_cast<size_t>(got));
      else if (got == 0 || (errno != EINTR && errno != EAGAIN)) break;  // the other end is closed: the run ended
    }
    if (cancel.load() && !killed) {
      kill(pid, SIGTERM);
      killed = true;
    }
  }
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
  if (!pending.empty()) on_line(pending);
  return true;
}

}  // namespace

DumpOutcome run_dump_process(const DumpRequest& r, const DumpProgressFn& progress, const std::atomic<bool>& cancel) {
  DumpOutcome out;
  out.out_dir = r.out_dir;
  if (r.exe.empty()) {
    out.error = "The program to run for the dump is not known.";
    return out;
  }
  if (r.out_dir.empty() || exists_path(fs::path(r.out_dir))) {
    out.error = "The dump folder already exists. Move or delete it first.";
    return out;
  }
  const std::string partial = r.out_dir + ".partial";
  std::error_code ec;
  fs::remove_all(partial, ec);
  ec.clear();
  fs::create_directories(fs::path(r.out_dir).parent_path(), ec);

  const uint32_t frames = std::max<uint32_t>(1, r.frames);
  std::vector<std::string> kept;  // the lines worth showing
  std::string last_error;
  auto on_line = [&](const std::string& line) {
    const long f = frame_of_report(line);
    if (f >= 0) {
      const int pm = static_cast<int>(std::min<long>(990, (f + 1) * 1000L / static_cast<long>(frames)));
      progress(pm, "Frame " + std::to_string(f + 1) + " of " + std::to_string(frames));
      return;
    }
    if (starts_with(line, "dump-assets:") || starts_with(line, "dump-audio:")) {
      if (kept.size() < 40) kept.push_back(line);
    } else if (starts_with(line, "x2recomp:")) {
      last_error = line.substr(std::strlen("x2recomp:"));
      while (!last_error.empty() && last_error.front() == ' ') last_error.erase(0, 1);
    }
  };
  progress(0, "Starting the game");
  int code = 0;
  std::string err;
  const bool started = run_and_read(r.exe, dump_command_args(r, partial), on_line, cancel, code, err);
  auto drop_partial = [&] {
    std::error_code e;
    fs::remove_all(partial, e);
  };
  if (!started) {
    out.error = err;
    drop_partial();
    return out;
  }
  if (cancel.load()) {
    out.cancelled = true;
    drop_partial();
    return out;
  }
  if (code != 0) {
    out.error = last_error.empty() ? "The game run stopped with exit code " + std::to_string(code) + "." : last_error;
    drop_partial();
    return out;
  }
  if (!exists_path(fs::path(partial) / "manifest.json")) {
    out.error = "The run finished but wrote no dump. The game may have stopped early.";
    drop_partial();
    return out;
  }
  progress(995, "Saving the dump");
  fs::rename(partial, r.out_dir, ec);
  if (ec) {
    out.error = "Could not move the finished dump into place: " + ec.message();
    drop_partial();
    return out;
  }
  progress(1000, "Done");
  out.ok = true;
  out.lines = std::move(kept);
  return out;
}

std::string next_dump_dir(const std::string& dumps_root, const std::string& game) {
  for (int n = 1; n < 10000; ++n) {
    const fs::path p = fs::path(dumps_root) / dump_name(game, n);
    if (!exists_path(p) && !exists_path(fs::path(p.string() + ".partial"))) return p.string();
  }
  return (fs::path(dumps_root) / dump_name(game, 10000)).string();
}

std::string latest_dump_dir(const std::string& dumps_root, const std::string& game) {
  std::string best;
  for (int n = 1; n < 10000; ++n) {
    const fs::path p = fs::path(dumps_root) / dump_name(game, n);
    if (exists_path(p / "manifest.json")) best = p.string();
    else if (n > 1 && !exists_path(p)) break;
  }
  return best;
}

// ---- the job runner ----

PackJobs::~PackJobs() {
  cancel_.store(true);
  join();
}

void PackJobs::join() {
  if (th_.joinable()) th_.join();
}

std::string PackJobs::stage() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stage_;
}

bool PackJobs::start_check(const pack::CheckOptions& o) {
  if (busy_.exchange(true)) return false;
  join();
  kind_.store(Kind::Check);
  permille_.store(0);
  cancel_.store(false);
  {
    std::lock_guard<std::mutex> lock(mu_);
    stage_ = "Checking the pack";
    have_check_ = false;
  }
  th_ = std::thread([this, o] {
    CheckDone d;
    d.path = o.path;
    d.against = !o.against.empty();
    try {
      d.result = pack::check_asset_pack(o);
    } catch (const std::exception& e) {
      d.result = pack::CheckResult();
      d.result.exit_code = 2;
      d.result.report.add(pack::Severity::Error, o.path, "", "", std::string("the check stopped: ") + e.what());
    }
    permille_.store(1000);
    std::lock_guard<std::mutex> lock(mu_);
    check_ = std::move(d);
    have_check_ = true;
  });
  return true;
}

bool PackJobs::start_dump(const DumpRequest& r, DumpRunner runner) {
  if (busy_.exchange(true)) return false;
  join();
  kind_.store(Kind::Dump);
  permille_.store(0);
  cancel_.store(false);
  {
    std::lock_guard<std::mutex> lock(mu_);
    stage_ = "Starting the game";
    have_dump_ = false;
  }
  th_ = std::thread([this, r, runner = std::move(runner)] {
    DumpOutcome o;
    auto progress = [this](int pm, const std::string& stage) {
      permille_.store(std::clamp(pm, 0, 1000));
      std::lock_guard<std::mutex> lock(mu_);
      stage_ = stage;
    };
    try {
      o = runner(r, progress, cancel_);
    } catch (const std::exception& e) {
      o = DumpOutcome();
      o.error = std::string("The dump stopped: ") + e.what();
    }
    std::lock_guard<std::mutex> lock(mu_);
    dump_ = std::move(o);
    have_dump_ = true;
  });
  return true;
}

bool PackJobs::take_check(CheckDone& out) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!have_check_) return false;
    out = std::move(check_);
    have_check_ = false;
  }
  join();
  kind_.store(Kind::None);
  busy_.store(false);
  return true;
}

bool PackJobs::take_dump(DumpOutcome& out) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!have_dump_) return false;
    out = std::move(dump_);
    have_dump_ = false;
  }
  join();
  kind_.store(Kind::None);
  busy_.store(false);
  return true;
}

}  // namespace x2::ui
