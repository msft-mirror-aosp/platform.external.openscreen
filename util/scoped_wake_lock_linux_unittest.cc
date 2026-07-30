// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"
#include "platform/test/fake_clock.h"
#include "platform/test/fake_task_runner.h"
#include "util/scoped_wake_lock.h"

namespace openscreen {

// Forward declaration of the testing hooks.
void SetWakeLockPathsForTesting(const char* lock_path, const char* unlock_path);
void SetWakeLockClockForTesting(ClockNowFunctionPtr clock_fn);

namespace {

class ScopedWakeLockTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create unique temp file paths.
    auto temp_dir = std::filesystem::temp_directory_path();
    lock_path_ = (temp_dir / "openscreen_wakelock_test_lock").string();
    unlock_path_ = (temp_dir / "openscreen_wakelock_test_unlock").string();

    // Clean up any leftover files.
    std::filesystem::remove(lock_path_);
    std::filesystem::remove(unlock_path_);
    SetWakeLockPathsForTesting(lock_path_.c_str(), unlock_path_.c_str());
    SetWakeLockClockForTesting(&FakeClock::now);

    expected_lock_name_ = "openscreen.wakelock." + std::to_string(getpid());
  }

  void TearDown() override {
    // Execute any pending tasks (such as destruction tasks posted to the
    // runner).
    task_runner_.RunTasksUntilIdle();

    std::filesystem::remove(lock_path_);
    std::filesystem::remove(unlock_path_);
    SetWakeLockPathsForTesting("/sys/power/wake_lock",
                               "/sys/power/wake_unlock");
    SetWakeLockClockForTesting(&Clock::now);
  }

  std::string ReadFile(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
      return "";
    }
    std::string content;
    std::string line;
    while (std::getline(ifs, line)) {
      if (!content.empty()) {
        content += "\n";
      }
      content += line;
    }
    return content;
  }

  FakeClock clock_{Clock::now()};
  FakeTaskRunner task_runner_{clock_};
  std::string lock_path_;
  std::string unlock_path_;
  std::string expected_lock_name_;
};

TEST_F(ScopedWakeLockTest, AcquireAndRelease) {
  {
    ScopedWakeLockPtr lock = ScopedWakeLock::Create(task_runner_);
    EXPECT_TRUE(lock);

    // The lock is acquired synchronously in the constructor.
    EXPECT_EQ(ReadFile(lock_path_), expected_lock_name_ + " 10000000000");
    EXPECT_TRUE(ReadFile(unlock_path_).empty());
  }

  // Lock destruction is asynchronous (via TaskRunnerDeleter), so it won't
  // execute the destructor and ReleaseWakeLock until the task runner runs.
  EXPECT_TRUE(ReadFile(unlock_path_).empty());

  task_runner_.RunTasksUntilIdle();
  EXPECT_EQ(ReadFile(unlock_path_), expected_lock_name_);
}

TEST_F(ScopedWakeLockTest, ReferenceCounting) {
  {
    ScopedWakeLockPtr lock1 = ScopedWakeLock::Create(task_runner_);
    EXPECT_EQ(ReadFile(lock_path_), expected_lock_name_ + " 10000000000");

    // Clear the file to verify if a second lock writes to it again.
    std::filesystem::remove(lock_path_);

    {
      ScopedWakeLockPtr lock2 = ScopedWakeLock::Create(task_runner_);
      // Should NOT have written to the file again because ref count > 1.
      EXPECT_TRUE(ReadFile(lock_path_).empty());
    }

    // lock2 destroyed. Its deletion task is posted but not yet run, but even if
    // it runs, it should NOT have released because lock1 is still alive.
    task_runner_.RunTasksUntilIdle();
    EXPECT_TRUE(ReadFile(unlock_path_).empty());
  }

  // lock1 destroyed, should release now (after running the deletion task).
  task_runner_.RunTasksUntilIdle();
  EXPECT_EQ(ReadFile(unlock_path_), expected_lock_name_);
}

TEST_F(ScopedWakeLockTest, HeartbeatRenewal) {
  ScopedWakeLockPtr lock = ScopedWakeLock::Create(task_runner_);
  std::string expected_content = expected_lock_name_ + " 10000000000";
  EXPECT_EQ(ReadFile(lock_path_), expected_content);

  // Clear the lock file to verify heartbeat write.
  std::filesystem::remove(lock_path_);

  // Run the initial ScheduleHeartbeat task.
  task_runner_.RunTasksUntilIdle();

  // Advance time by 4 seconds. Heartbeat is at 5 seconds, so it shouldn't run.
  clock_.Advance(std::chrono::seconds(4));
  task_runner_.RunTasksUntilIdle();
  EXPECT_TRUE(ReadFile(lock_path_).empty());

  // Advance time by another 1.5 seconds (total 5.5s). Heartbeat should run and
  // renew.
  clock_.Advance(std::chrono::milliseconds(1500));
  task_runner_.RunTasksUntilIdle();
  EXPECT_EQ(ReadFile(lock_path_), expected_content);

  // Clean up lock to prevent static state pollution for subsequent tests.
  lock.reset();
  task_runner_.RunTasksUntilIdle();
}

TEST_F(ScopedWakeLockTest, HeartbeatStopsOnRelease) {
  {
    ScopedWakeLockPtr lock = ScopedWakeLock::Create(task_runner_);
    EXPECT_FALSE(ReadFile(lock_path_).empty());
    std::filesystem::remove(lock_path_);
  }  // lock unique_ptr is destroyed (delete task posted)

  task_runner_.RunTasksUntilIdle();  // Runs the deletion task, releasing lock.
  EXPECT_EQ(ReadFile(unlock_path_), expected_lock_name_);

  // Clear files to verify no further writes.
  std::filesystem::remove(lock_path_);
  std::filesystem::remove(unlock_path_);

  // Advance time by 10 seconds. No heartbeat should run anymore.
  clock_.Advance(std::chrono::seconds(10));
  task_runner_.RunTasksUntilIdle();
  EXPECT_TRUE(ReadFile(lock_path_).empty());
  EXPECT_TRUE(ReadFile(unlock_path_).empty());
}

}  // namespace
}  // namespace openscreen
