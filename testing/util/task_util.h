// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TESTING_UTIL_TASK_UTIL_H_
#define TESTING_UTIL_TASK_UTIL_H_

#include <future>
#include <thread>
#include <utility>

#include "gtest/gtest.h"
#include "platform/api/task_runner.h"
#include "platform/api/time.h"
#include "util/osp_logging.h"

namespace openscreen {

template <typename Cond>
void WaitForCondition(Cond condition,
                      Clock::duration delay = std::chrono::milliseconds(250),
                      int max_attempts = 8) {
  int attempts = 1;
  do {
    OSP_LOG_INFO << "--- Checking condition, attempt " << attempts << "/"
                 << max_attempts;
    if (condition()) {
      break;
    }
    std::this_thread::sleep_for(delay);
  } while (attempts++ < max_attempts);
  ASSERT_TRUE(condition());
}

// Helper to run a synchronous task on the TaskRunner and get its return value.
template <typename Functor>
auto RunOnTaskRunner(TaskRunner& task_runner,
                     Functor&& f,
                     std::chrono::milliseconds timeout =
                         std::chrono::seconds(5)) -> decltype(f()) {
  using ReturnType = decltype(f());

  std::packaged_task<ReturnType()> task(std::forward<Functor>(f));
  auto future = task.get_future();
  task_runner.PostTask([task = std::move(task)]() mutable { task(); });

  const auto status = future.wait_for(timeout);
  if (status != std::future_status::ready) {
    OSP_LOG_FATAL << "Task timed out on TaskRunner";
  }
  return future.get();
}

}  // namespace openscreen

#endif  // TESTING_UTIL_TASK_UTIL_H_
