// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "worker.h"

#include <utility>

namespace x2::ui {

void JobContext::progress(int permille, std::string_view stage) {
  std::lock_guard<std::mutex> lock(mu_);
  if (permille > 1000) permille = 1000;
  if (permille > permille_) permille_ = permille;
  stage_.assign(stage);
}

void JobContext::message(Severity severity, std::string text) {
  std::lock_guard<std::mutex> lock(mu_);
  pending_.push_back({severity, std::move(text)});
}

Worker::~Worker() {
  if (thread_.joinable()) {
    ctx_.cancel_.store(true);
    thread_.join();
  }
}

bool Worker::start(std::function<void(JobContext&)> job) {
  if (state_.load() != State::Idle) return false;
  {
    std::lock_guard<std::mutex> lock(ctx_.mu_);
    ctx_.permille_ = 0;
    ctx_.stage_.clear();
    ctx_.pending_.clear();
  }
  ctx_.cancel_.store(false);
  state_.store(State::Running);
  thread_ = std::thread([this, job = std::move(job)] {
    try {
      job(ctx_);
    } catch (...) {
      ctx_.message(Severity::Error, "The job stopped because of an unexpected error.");
    }
    state_.store(State::Finished);
  });
  return true;
}

void Worker::cancel() { ctx_.cancel_.store(true); }

JobView Worker::poll(std::vector<Message>& out) {
  JobView v;
  v.running = state_.load() == State::Running;
  std::lock_guard<std::mutex> lock(ctx_.mu_);
  v.permille = ctx_.permille_;
  v.stage = ctx_.stage_;
  for (Message& m : ctx_.pending_) out.push_back(std::move(m));
  ctx_.pending_.clear();
  return v;
}

bool Worker::collect() {
  if (state_.load() != State::Finished) return false;
  if (thread_.joinable()) thread_.join();
  state_.store(State::Idle);
  return true;
}

}  // namespace x2::ui
