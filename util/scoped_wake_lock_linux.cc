// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "platform/api/task_runner.h"
#include "util/alarm.h"
#include "util/chrono_helpers.h"
#include "util/no_destructor.h"
#include "util/osp_logging.h"
#include "util/scoped_wake_lock.h"
#include "util/thread_annotations.h"

namespace openscreen {

namespace {

constexpr auto kLockTimeout = std::chrono::seconds(10);

// This mutex guards the reference count to ensure atomic transitions between
// 0 and 1 references, which correspond to acquiring and releasing the physical
// wake lock.
std::mutex g_wake_lock_mutex;

// Use atomic pointers to allow thread-safe overriding in tests without
// needing to lock the mutex just to read the paths or clock.
std::atomic<const char*> g_wake_lock_path{"/sys/power/wake_lock"};
std::atomic<const char*> g_wake_unlock_path{"/sys/power/wake_unlock"};
std::atomic<ClockNowFunctionPtr> g_clock_now{&Clock::now};

std::string_view GetLockName() {
  // Initialized exactly once on the first call. Thread-safe by C++11/C++20
  // standards.
  static const NoDestructor<std::string> kLockName("openscreen.wakelock." +
                                                   std::to_string(getpid()));
  return *kLockName;
}

void WriteToSysfs(const std::filesystem::path& path, std::string_view content) {
  // std::ios::app ensures we append, mimicking O_APPEND
  std::ofstream stream(path, std::ios::out | std::ios::app);
  if (!stream) {
    OSP_LOG_WARN << "Failed to open wake lock path: " << path;
    return;
  }

  // Write the string view. std::ofstream handles partial writes and
  // buffering automatically.
  stream << content;
  if (stream.bad()) {
    OSP_LOG_WARN << "Failed to write to wake lock path: " << path;
  }
}

void AcquireWakeLockLocked() OSP_EXCLUSIVE_LOCKS_REQUIRED(g_wake_lock_mutex) {
  const std::string content =
      std::format("{} {}", GetLockName(), to_nanoseconds(kLockTimeout).count());

  WriteToSysfs(g_wake_lock_path.load(), content);
}

void ReleaseWakeLockLocked() OSP_EXCLUSIVE_LOCKS_REQUIRED(g_wake_lock_mutex) {
  WriteToSysfs(g_wake_unlock_path.load(), GetLockName());
}

}  // namespace

void SetWakeLockPathsForTesting(const char* lock_path,
                                const char* unlock_path) {
  g_wake_lock_path.store(lock_path);
  g_wake_unlock_path.store(unlock_path);
}

void SetWakeLockClockForTesting(ClockNowFunctionPtr clock_fn) {
  g_clock_now.store(clock_fn);
}

class ScopedWakeLockLinux : public ScopedWakeLock {
 public:
  explicit ScopedWakeLockLinux(TaskRunner& task_runner);
  ~ScopedWakeLockLinux() override;

 private:
  TaskRunner& task_runner_;
  Alarm heartbeat_alarm_;

  static int reference_count_ OSP_GUARDED_BY(g_wake_lock_mutex);

  void ScheduleHeartbeat();
};

int ScopedWakeLockLinux::reference_count_ = 0;

ScopedWakeLockPtr ScopedWakeLock::Create(TaskRunner& task_runner) {
  return TaskRunnerDeleter::MakeUnique<ScopedWakeLockLinux>(task_runner,
                                                            task_runner);
}

ScopedWakeLockLinux::ScopedWakeLockLinux(TaskRunner& task_runner)
    : task_runner_(task_runner),
      heartbeat_alarm_(g_clock_now.load(), task_runner_) {
  std::lock_guard<std::mutex> lock(g_wake_lock_mutex);
  if (reference_count_++ == 0) {
    AcquireWakeLockLocked();
  }
  task_runner_.PostTask([this] { ScheduleHeartbeat(); });
}

ScopedWakeLockLinux::~ScopedWakeLockLinux() {
  std::lock_guard<std::mutex> lock(g_wake_lock_mutex);
  if (--reference_count_ == 0) {
    ReleaseWakeLockLocked();
  }
}

// The Linux sysfs wake lock interface (/sys/power/wake_lock) does not
// automatically clean up locks when a process exits. To prevent leaking locks
// on crash, we acquire the lock with a short timeout (10 seconds) and run a
// heartbeat timer to periodically renew it.
//
// When renewing, we write to the wake lock file again. This resets the timeout
// in the kernel without needing to increment our logical reference count, as we
// are just maintaining the existing lock.
void ScopedWakeLockLinux::ScheduleHeartbeat() {
  heartbeat_alarm_.ScheduleFromNow(
      [this] {
        {
          std::lock_guard<std::mutex> lock(g_wake_lock_mutex);
          AcquireWakeLockLocked();
        }
        ScheduleHeartbeat();
      },
      std::chrono::seconds(5));
}

}  // namespace openscreen
