// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// One background job at a time (the launcher's import runs here, so the window stays alive and the gamepad still works). The job reports
// progress and messages through a JobContext and polls cancelled(); the UI thread polls the Worker once per frame and never touches
// anything the job owns. No SDL: the tests drive it directly.
#pragma once
#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ui_widgets.h"

namespace x2::ui {

class Worker;

// What a job sees. Thread-safe: the job calls it on its own thread, the UI polls the Worker on another.
class JobContext {
 public:
  // permille only ever rises (a lower value is ignored), 0..1000.
  void progress(int permille, std::string_view stage);
  void message(Severity severity, std::string text);
  bool cancelled() const { return cancel_.load(std::memory_order_relaxed); }
  const std::atomic<bool>* cancel_flag() const { return &cancel_; }

 private:
  friend class Worker;
  std::atomic<bool> cancel_{false};
  std::mutex mu_;
  int permille_ = 0;
  std::string stage_;
  std::vector<Message> pending_;
};

// What the UI thread reads each frame.
struct JobView {
  bool running = false;
  int permille = 0;
  std::string stage;
};

class Worker {
 public:
  Worker() = default;
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;
  ~Worker();  // cancels a running job and waits for it

  // False (and nothing started) while a job is running or its result has not been collected.
  bool start(std::function<void(JobContext&)> job);
  bool busy() const { return state_.load() != State::Idle; }
  bool running() const { return state_.load() == State::Running; }
  void cancel();
  // Moves the messages the job has posted since the last call onto `out`, and returns the progress.
  JobView poll(std::vector<Message>& out);
  // True once, when the job has returned: the thread is joined and the worker can start another job.
  bool collect();

 private:
  enum class State { Idle, Running, Finished };
  std::atomic<State> state_{State::Idle};
  JobContext ctx_;
  std::thread thread_;
};

}  // namespace x2::ui
