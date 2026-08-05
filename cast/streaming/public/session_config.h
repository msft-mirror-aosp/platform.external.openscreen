// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STREAMING_PUBLIC_SESSION_CONFIG_H_
#define CAST_STREAMING_PUBLIC_SESSION_CONFIG_H_

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

#include "cast/streaming/public/constants.h"
#include "cast/streaming/ssrc.h"

namespace openscreen::cast {

// Common streaming configuration, established from the OFFER/ANSWER exchange,
// that the Sender and Receiver are both assuming.
struct SessionConfig final {
  SessionConfig(Ssrc sender_ssrc,
                Ssrc receiver_ssrc,
                int rtp_timebase,
                int channels,
                std::chrono::milliseconds target_playout_delay,
                std::array<uint8_t, 16> aes_secret_key,
                std::array<uint8_t, 16> aes_iv_mask,
                bool is_pli_enabled = false,
                StreamType stream_type = StreamType::kUnknown,
                bool are_receiver_event_logs_enabled = true,
                bool allow_skip_to_keyframe = false);
  SessionConfig(const SessionConfig& other);
  SessionConfig(SessionConfig&& other) noexcept;
  SessionConfig& operator=(const SessionConfig& other);
  SessionConfig& operator=(SessionConfig&& other) noexcept;
  ~SessionConfig();

  bool IsValid() const;

  // The sender and receiver's SSRC identifiers. Note: SSRC identifiers
  // are defined as unsigned 32 bit integers here:
  // https://tools.ietf.org/html/rfc5576#page-5
  Ssrc sender_ssrc = 0;
  Ssrc receiver_ssrc = 0;

  // RTP timebase: The number of RTP units advanced per second. For audio,
  // this is the sampling rate. For video, this is 90 kHz by convention.
  int rtp_timebase = 90000;

  // Number of channels. Must be 1 for video, for audio typically 2.
  int channels = 1;

  // Initial target playout delay.
  std::chrono::milliseconds target_playout_delay;

  // The AES-128 crypto key and initialization vector.
  std::array<uint8_t, 16> aes_secret_key{};
  std::array<uint8_t, 16> aes_iv_mask{};

  // Whether picture loss indication (PLI) should be used for this session.
  bool is_pli_enabled = false;

  // The type (e.g. audio or video) of the stream.
  StreamType stream_type = StreamType::kUnknown;

  // Whether RTCP event logs from the Receiver are enabled. These are used for
  // generating statistics. It is recommended that this generally be true.
  bool are_receiver_event_logs_enabled = true;

  // Optional optimization to skip incomplete/late frames on packet loss.
  // Default is false (preserves original behavior).
  bool allow_skip_to_keyframe = false;

  // Optional override for the keyframe timeout.
  // If > 0, overrides the default.
  std::optional<std::chrono::milliseconds> sender_keyframe_cooldown;

  // The interval to rate-limit proactive PLI requests for late frames.
  // If > 0, proactive PLIs will be sent for late frames. Note that PLI requests
  // still require `is_pli_enabled` to be true.
  std::optional<std::chrono::milliseconds> receiver_proactive_pli_interval;

  // Optional override for the maximum in-flight media duration.
  std::optional<std::chrono::milliseconds> max_in_flight_media_duration;
};

}  // namespace openscreen::cast

#endif  // CAST_STREAMING_PUBLIC_SESSION_CONFIG_H_
