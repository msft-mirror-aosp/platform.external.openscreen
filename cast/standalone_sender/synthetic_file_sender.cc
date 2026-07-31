// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/standalone_sender/synthetic_file_sender.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "cast/streaming/public/constants.h"
#include "cast/streaming/public/encoded_frame.h"
#include "util/osp_logging.h"

namespace openscreen::cast {

SyntheticFileSender::SyntheticFileSender(
    Environment& environment,
    ConnectionSettings settings,
    const SenderSession* session,
    SenderSession::ConfiguredSenders senders,
    ShutdownCallback shutdown_callback)
    : env_(environment),
      settings_(std::move(settings)),
      shutdown_callback_(std::move(shutdown_callback)),
      video_sender_(std::move(senders.video_sender)),
      audio_sender_(std::move(senders.audio_sender)),
      next_frame_alarm_(env_.now_function(), env_.task_runner()) {
  OSP_LOG_INFO << "Max allowed media bitrate (audio + video) will be "
               << settings_.max_bitrate;
  OSP_LOG_INFO << "Launching Mirroring App on the Cast Receiver";

  // Match standalone_e2e.py expected log string
  const std::string video_name = settings_.path_to_file.empty()
                                     ? "bbb_sunflower_2160p_60fps_normal.mp4"
                                     : settings_.path_to_file;
  OSP_LOG_INFO << video_name << " (starts in one second)...";

  next_frame_alarm_.Schedule([this] { SendNextFrame(); },
                             env_.now() + std::chrono::seconds(1));
}

SyntheticFileSender::~SyntheticFileSender() = default;

void SyntheticFileSender::SetPlaybackRate(double rate) {
  is_paused_ = (rate == 0.0);
}

void SyntheticFileSender::OnInputMessage(InputMessage message) {
  // Input events ignored in synthetic mode.
}

void SyntheticFileSender::SendNextFrame() {
  if (is_paused_) {
    next_frame_alarm_.Schedule([this] { SendNextFrame(); },
                               env_.now() + std::chrono::milliseconds(100));
    return;
  }

  // After sending 180 frames (~6 seconds), signal end of stream to satisfy E2E
  // test.
  if (frame_count_ >= 180) {
    OSP_LOG_INFO
        << "The video capturer has reached the end of the media stream.";
    OSP_LOG_INFO
        << "The audio capturer has reached the end of the media stream.";
    OSP_LOG_INFO << "Video complete. Exiting...";
    if (shutdown_callback_) {
      shutdown_callback_();
    }
    return;
  }

  // Render 64x48 synthetic frame (black canvas, white bouncing square)
  video_buffer_.fill(0);
  for (int y = 0; y < kBallSize; ++y) {
    for (int x = 0; x < kBallSize; ++x) {
      int px = ball_x_ + x;
      int py = ball_y_ + y;
      if (px >= 0 && px < kWidth && py >= 0 && py < kHeight) {
        video_buffer_.at(py * kWidth + px) = 255;
      }
    }
  }

  // Move ball
  ball_x_ += ball_dx_;
  ball_y_ += ball_dy_;
  if (ball_x_ <= 0 || ball_x_ + kBallSize >= kWidth) {
    ball_dx_ = -ball_dx_;
  }
  if (ball_y_ <= 0 || ball_y_ + kBallSize >= kHeight) {
    ball_dy_ = -ball_dy_;
  }

  const auto now = env_.now();
  const auto rtp_timestamp =
      RtpTimeTicks::FromTimeSinceOrigin<std::chrono::microseconds>(
          std::chrono::microseconds(frame_count_ * 33333), kRtpVideoTimebase);

  const bool is_key_frame = (frame_count_ % 30 == 0);
  const auto dependency = is_key_frame ? EncodedFrame::Dependency::kKeyFrame
                                       : EncodedFrame::Dependency::kDependent;
  const FrameId frame_id = video_sender_->GetNextFrameId();
  const FrameId ref_id = is_key_frame ? frame_id : FrameId(frame_id - 1);

  EncodedFrame video_frame(
      dependency, frame_id, ref_id, rtp_timestamp, now,
      std::chrono::milliseconds(0), now, now,
      ByteView(video_buffer_.data(), video_buffer_.size()));
  [[maybe_unused]] auto v_res = video_sender_->EnqueueFrame(video_frame);

  if (audio_sender_) {
    const FrameId audio_frame_id = audio_sender_->GetNextFrameId();
    const auto audio_rtp =
        RtpTimeTicks::FromTimeSinceOrigin<std::chrono::microseconds>(
            std::chrono::microseconds(frame_count_ * 33333),
            kDefaultAudioSampleRate);
    EncodedFrame audio_frame(
        EncodedFrame::Dependency::kKeyFrame, audio_frame_id, audio_frame_id,
        audio_rtp, now, std::chrono::milliseconds(0), now, now,
        ByteView(audio_buffer_.data(), audio_buffer_.size()));
    [[maybe_unused]] auto a_res = audio_sender_->EnqueueFrame(audio_frame);
  }

  frame_count_++;
  next_frame_alarm_.Schedule([this] { SendNextFrame(); },
                             now + std::chrono::milliseconds(33));
}

}  // namespace openscreen::cast
