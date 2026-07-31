// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/standalone_sender/file_sender.h"

#include <utility>

#include "build/build_config.h"

#if defined(CAST_STANDALONE_SENDER_HAVE_EXTERNAL_LIBS)
#include "cast/standalone_sender/looping_file_sender.h"
#else
#include "cast/standalone_sender/synthetic_file_sender.h"
#endif

namespace openscreen::cast {

// static
std::unique_ptr<FileSender> FileSender::Create(
    Environment& environment,
    ConnectionSettings settings,
    const SenderSession* session,
    SenderSession::ConfiguredSenders senders,
    ShutdownCallback shutdown_callback) {
#if defined(CAST_STANDALONE_SENDER_HAVE_EXTERNAL_LIBS)
  return std::make_unique<LoopingFileSender>(environment, std::move(settings),
                                             session, std::move(senders),
                                             std::move(shutdown_callback));
#else
  return std::make_unique<SyntheticFileSender>(environment, std::move(settings),
                                               session, std::move(senders),
                                               std::move(shutdown_callback));
#endif
}

}  // namespace openscreen::cast
