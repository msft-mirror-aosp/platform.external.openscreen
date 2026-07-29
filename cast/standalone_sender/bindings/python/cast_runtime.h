// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STANDALONE_SENDER_BINDINGS_PYTHON_CAST_RUNTIME_H_
#define CAST_STANDALONE_SENDER_BINDINGS_PYTHON_CAST_RUNTIME_H_

#include <thread>

#include "platform/impl/task_runner.h"
#include "util/raw_ptr.h"

namespace openscreen {
class TaskRunnerImpl;
}

namespace openscreen::cast {

class CastRuntime {
 public:
  CastRuntime();
  ~CastRuntime();

  TaskRunnerImpl* task_runner() { return task_runner_.get(); }

 private:
  // Note: CastRuntime does not own the task runner. It is owned by the
  // PlatformClientPosix. Returning this pointer is safe because the
  // PlatformClientPosix is guaranteed to outlive this CastRuntime instance.
  raw_ptr<TaskRunnerImpl> task_runner_ = nullptr;
  std::thread event_loop_thread_;
};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_BINDINGS_PYTHON_CAST_RUNTIME_H_
