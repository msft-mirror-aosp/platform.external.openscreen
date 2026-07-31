// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STANDALONE_SENDER_FILE_SENDER_H_
#define CAST_STANDALONE_SENDER_FILE_SENDER_H_

#include <functional>
#include <memory>

#include "cast/standalone_sender/connection_settings.h"
#include "cast/streaming/public/sender_session.h"

namespace openscreen {
class Environment;

namespace cast {

class InputMessage;

class FileSender {
 public:
  using ShutdownCallback = std::function<void()>;

  static std::unique_ptr<FileSender> Create(
      Environment& environment,
      ConnectionSettings settings,
      const SenderSession* session,
      SenderSession::ConfiguredSenders senders,
      ShutdownCallback shutdown_callback);

  virtual ~FileSender() = default;

  virtual void SetPlaybackRate(double rate) = 0;
  virtual void OnInputMessage(InputMessage message) = 0;
};

}  // namespace cast
}  // namespace openscreen

#endif  // CAST_STANDALONE_SENDER_FILE_SENDER_H_
