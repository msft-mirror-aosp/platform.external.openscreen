// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STANDALONE_SENDER_SYNTHETIC_FILE_SENDER_H_
#define CAST_STANDALONE_SENDER_SYNTHETIC_FILE_SENDER_H_

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "cast/standalone_sender/connection_settings.h"
#include "cast/standalone_sender/file_sender.h"
#include "cast/streaming/public/environment.h"
#include "cast/streaming/public/sender.h"
#include "cast/streaming/public/sender_session.h"
#include "util/alarm.h"

namespace openscreen::cast {

// Generates synthetic video and audio frames in memory and streams them
// over Cast Streaming when external libraries (FFMPEG/libvpx/libopus) are
// missing.
class SyntheticFileSender final : public FileSender {
 public:
  static constexpr int kWidth = 64;
  static constexpr int kHeight = 48;
  static constexpr size_t kFrameSizeBytes = kWidth * kHeight;  // 3072 bytes
  static constexpr size_t kAudioFrameSizeBytes = 960;

  SyntheticFileSender(Environment& environment,
                      ConnectionSettings settings,
                      const SenderSession* session,
                      SenderSession::ConfiguredSenders senders,
                      ShutdownCallback shutdown_callback);

  ~SyntheticFileSender() override;

  void SetPlaybackRate(double rate) override;
  void OnInputMessage(InputMessage message) override;

 private:
  void SendNextFrame();

  Environment& env_;
  const ConnectionSettings settings_;
  ShutdownCallback shutdown_callback_;

  std::unique_ptr<Sender> video_sender_;
  std::unique_ptr<Sender> audio_sender_;

  Alarm next_frame_alarm_;
  int frame_count_ = 0;
  bool is_paused_ = false;

  // Bouncing ball animation state
  int ball_x_ = 10;
  int ball_y_ = 10;
  int ball_dx_ = 2;
  int ball_dy_ = 1;
  static constexpr int kBallSize = 8;

  std::array<uint8_t, kFrameSizeBytes> video_buffer_{};
  std::array<uint8_t, kAudioFrameSizeBytes> audio_buffer_{};
};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_SYNTHETIC_FILE_SENDER_H_
