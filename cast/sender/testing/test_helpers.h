// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_SENDER_TESTING_TEST_HELPERS_H_
#define CAST_SENDER_TESTING_TEST_HELPERS_H_

#include <cstdint>
#include <string>

#include "cast/sender/channel/message_util.h"

namespace openscreen::cast {
namespace proto {
class CastMessage;
}

void VerifyAppAvailabilityRequest(const proto::CastMessage& message,
                                  const std::string& expected_app_id,
                                  int* request_id_out,
                                  std::string* sender_id_out);
void VerifyAppAvailabilityRequest(const proto::CastMessage& message,
                                  std::string* app_id_out,
                                  int* request_id_out,
                                  std::string* sender_id_out);

proto::CastMessage CreateAppAvailableResponseChecked(
    int request_id,
    const std::string& sender_id,
    const std::string& app_id);
proto::CastMessage CreateAppUnavailableResponseChecked(
    int request_id,
    const std::string& sender_id,
    const std::string& app_id);

void VerifyLaunchRequest(const proto::CastMessage& message,
                         const std::string& expected_app_id,
                         int* request_id_out,
                         std::string* sender_id_out);

void VerifyStopRequest(const proto::CastMessage& message,
                       const std::string& expected_session_id,
                       int* request_id_out,
                       std::string* sender_id_out);

proto::CastMessage CreateLaunchStatusResponse(int request_id,
                                              const std::string& sender_id);
// Like CreateLaunchStatusResponse(), but reports that the receiver is still
// waiting on the user to approve or reject the cast request, rather than a
// terminal success.
proto::CastMessage CreateLaunchPendingUserAuthResponse(
    int request_id,
    const std::string& sender_id);
proto::CastMessage CreateLaunchErrorResponse(int request_id,
                                             const std::string& sender_id,
                                             const std::string& reason);
// Like CreateLaunchErrorResponse(), but reports a specific `extendedError`
// value (e.g. kMessageValueUserNotAllowed) instead of a free-text `reason`.
proto::CastMessage CreateLaunchExtendedErrorResponse(
    int request_id,
    const std::string& sender_id,
    const std::string& extended_error);
proto::CastMessage CreateStopErrorResponse(int request_id,
                                           const std::string& sender_id,
                                           const std::string& reason);

// Builds a RECEIVER_STATUS response listing `running_session_id` as the
// running application's session id, or listing no applications at
// all if `running_session_id` is empty.
proto::CastMessage CreateReceiverStatusResponse(
    int request_id,
    const std::string& sender_id,
    const std::string& running_session_id);

}  // namespace openscreen::cast

#endif  // CAST_SENDER_TESTING_TEST_HELPERS_H_
