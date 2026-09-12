// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/sender/cast_platform_client.h"

#include <utility>

#include "cast/common/channel/testing/fake_cast_socket.h"
#include "cast/common/channel/testing/mock_socket_error_handler.h"
#include "cast/common/channel/virtual_connection_router.h"
#include "cast/common/public/receiver_info.h"
#include "cast/sender/testing/test_helpers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "platform/test/fake_clock.h"
#include "platform/test/fake_task_runner.h"
#include "util/json/json_serialization.h"
#include "util/json/json_value.h"
#include "util/raw_ptr.h"

namespace openscreen::cast {

using proto::CastMessage;

using ::testing::_;

class CastPlatformClientTest : public ::testing::Test {
 public:
  void SetUp() override {
    const int socket_id = fake_cast_socket_pair_.socket->socket_id();
    router_.TakeSocket(&mock_error_handler_,
                       std::move(fake_cast_socket_pair_.socket));

    receiver_.v4_address = IPAddress{192, 168, 0, 17};
    receiver_.port = 4434;
    receiver_.unique_id = "receiverId1";
    platform_client_.AddOrUpdateReceiver(receiver_, socket_id);
  }

 protected:
  CastSocket& peer_socket() { return *fake_cast_socket_pair_.peer_socket; }
  MockCastSocketClient& peer_client() {
    return fake_cast_socket_pair_.mock_peer_client;
  }

  // Expects the next message sent to the receiver to be an app-availability
  // request for `app_id`, capturing the request id and sender id.
  void ExpectAppAvailabilityRequest(const std::string& app_id,
                                    int* request_id,
                                    std::string* sender_id) {
    EXPECT_CALL(peer_client(), OnMessage(_, _))
        .WillOnce([app_id, request_id, sender_id](CastSocket* socket,
                                                  CastMessage message) {
          VerifyAppAvailabilityRequest(message, app_id, request_id, sender_id);
        });
  }

  // Expects the next message sent to the receiver to be a launch request for
  // `app_id`, capturing the request id and sender id.
  void ExpectLaunchRequest(const std::string& app_id,
                           int* request_id,
                           std::string* sender_id) {
    EXPECT_CALL(peer_client(), OnMessage(_, _))
        .WillOnce([app_id, request_id, sender_id](CastSocket* socket,
                                                  CastMessage message) {
          VerifyLaunchRequest(message, app_id, request_id, sender_id);
        });
  }

  // Expects the next message sent to the receiver to be a stop request for
  // `session_id`, capturing the request id and sender id.
  void ExpectStopRequest(const std::string& session_id,
                         int* request_id,
                         std::string* sender_id) {
    EXPECT_CALL(peer_client(), OnMessage(_, _))
        .WillOnce([session_id, request_id, sender_id](CastSocket* socket,
                                                      CastMessage message) {
          VerifyStopRequest(message, session_id, request_id, sender_id);
        });
  }

  // Launches a session for `app_id` on `receiver_id` with no app params.
  std::optional<int> LaunchSession(
      std::string_view receiver_id,
      std::string_view app_id,
      CastPlatformClient::LaunchSessionCallback callback) {
    return platform_client_.LaunchSession(receiver_id, app_id, Json::Value(),
                                          std::move(callback));
  }

  FakeCastSocketPair fake_cast_socket_pair_;
  MockSocketErrorHandler mock_error_handler_;
  VirtualConnectionRouter router_;
  FakeClock clock_{Clock::now()};
  FakeTaskRunner task_runner_{clock_};
  CastPlatformClient platform_client_{router_, &FakeClock::now, task_runner_};
  ReceiverInfo receiver_;
};

TEST_F(CastPlatformClientTest, AppAvailability) {
  int request_id = -1;
  std::string sender_id;
  ExpectAppAvailabilityRequest("AAAAAAAA", &request_id, &sender_id);
  bool ran = false;
  platform_client_.RequestAppAvailability(
      "receiverId1", "AAAAAAAA",
      [&ran](std::string_view app_id, AppAvailabilityResult availability) {
        EXPECT_EQ("AAAAAAAA", app_id);
        EXPECT_EQ(availability, AppAvailabilityResult::kAvailable);
        ran = true;
      });

  CastMessage availability_response =
      CreateAppAvailableResponseChecked(request_id, sender_id, "AAAAAAAA");
  EXPECT_TRUE(peer_socket().Send(availability_response).ok());
  EXPECT_TRUE(ran);

  // NOTE: Callback should only fire once, so it should not fire again here.
  ran = false;
  EXPECT_TRUE(peer_socket().Send(availability_response).ok());
  EXPECT_FALSE(ran);
}

TEST_F(CastPlatformClientTest, CancelRequest) {
  int request_id = -1;
  std::string sender_id;
  ExpectAppAvailabilityRequest("AAAAAAAA", &request_id, &sender_id);
  std::optional<int> maybe_request_id = platform_client_.RequestAppAvailability(
      "receiverId1", "AAAAAAAA",
      [](std::string_view app_id, AppAvailabilityResult availability) {
        EXPECT_TRUE(false);
      });
  ASSERT_TRUE(maybe_request_id);
  int local_request_id = maybe_request_id.value();
  platform_client_.CancelRequest(local_request_id);

  CastMessage availability_response =
      CreateAppAvailableResponseChecked(request_id, sender_id, "AAAAAAAA");
  EXPECT_TRUE(peer_socket().Send(availability_response).ok());
}

// LAUNCH_STATUS with USER_ALLOWED does not resolve the request. The
// RECEIVER_STATUS that follows does.
TEST_F(CastPlatformClientTest, LaunchSessionSuccess) {
  int request_id = -1;
  std::string sender_id;
  ExpectLaunchRequest("AAAAAAAA", &request_id, &sender_id);
  bool ran = false;
  LaunchSession("receiverId1", "AAAAAAAA",
                [&ran](ErrorOr<CastPlatformClient::ReceiverStatus> result) {
                  ASSERT_TRUE(result.is_value());
                  EXPECT_EQ(result.value().session_id, "session1");
                  ran = true;
                });

  CastMessage launch_status_response =
      CreateLaunchStatusResponse(request_id, sender_id);
  EXPECT_TRUE(peer_socket().Send(launch_status_response).ok());
  EXPECT_FALSE(ran);

  CastMessage receiver_status_response =
      CreateReceiverStatusResponse(request_id, sender_id, "session1");
  EXPECT_TRUE(peer_socket().Send(receiver_status_response).ok());
  EXPECT_TRUE(ran);
}

TEST_F(CastPlatformClientTest, LaunchSessionError) {
  int request_id = -1;
  std::string sender_id;
  ExpectLaunchRequest("AAAAAAAA", &request_id, &sender_id);
  bool ran = false;
  LaunchSession("receiverId1", "AAAAAAAA",
                [&ran](ErrorOr<CastPlatformClient::ReceiverStatus> result) {
                  EXPECT_TRUE(result.is_error());
                  ran = true;
                });

  CastMessage launch_response =
      CreateLaunchErrorResponse(request_id, sender_id, "NOT_FOUND");
  EXPECT_TRUE(peer_socket().Send(launch_response).ok());
  EXPECT_TRUE(ran);
}

// Some real receivers (observed on older firmware) skip LAUNCH_STATUS
// entirely and confirm a successful launch with a single RECEIVER_STATUS
// whose requestId already matches the LAUNCH request -- mirroring how
// Chromium's GetLaunchSessionResponse() treats this case.
TEST_F(CastPlatformClientTest, LaunchSessionSuccessViaBareReceiverStatus) {
  int request_id = -1;
  std::string sender_id;
  ExpectLaunchRequest("AAAAAAAA", &request_id, &sender_id);
  bool ran = false;
  LaunchSession("receiverId1", "AAAAAAAA",
                [&ran](ErrorOr<CastPlatformClient::ReceiverStatus> result) {
                  ASSERT_TRUE(result.is_value());
                  const CastPlatformClient::ReceiverStatus& status =
                      result.value();
                  EXPECT_EQ(status.session_id, "session1");
                  EXPECT_EQ(status.app_id, "AAAAAAAA");
                  EXPECT_EQ(status.transport_id, "session1-transport");
                  EXPECT_EQ(status.display_name, "Test App");
                  EXPECT_FALSE(status.is_idle_screen);
                  ran = true;
                });

  CastMessage launch_response =
      CreateReceiverStatusResponse(request_id, sender_id, "session1");
  EXPECT_TRUE(peer_socket().Send(launch_response).ok());
  EXPECT_TRUE(ran);
}

// Some receivers require the user to approve or reject the cast request on
// the receiver device, and report that state via a LAUNCH_STATUS with
// USER_PENDING_AUTHORIZATION before a later LAUNCH_STATUS confirms the user's
// choice.
TEST_F(CastPlatformClientTest, LaunchSessionPendingUserAuthThenAllowed) {
  int request_id = -1;
  std::string sender_id;
  ExpectLaunchRequest("AAAAAAAA", &request_id, &sender_id);
  bool ran = false;
  LaunchSession("receiverId1", "AAAAAAAA",
                [&ran](ErrorOr<CastPlatformClient::ReceiverStatus> result) {
                  EXPECT_TRUE(result.is_value());
                  ran = true;
                });

  CastMessage pending_response =
      CreateLaunchPendingUserAuthResponse(request_id, sender_id);
  EXPECT_TRUE(peer_socket().Send(pending_response).ok());
  EXPECT_FALSE(ran);

  // USER_ALLOWED still an intermediate stage
  CastMessage allowed_response =
      CreateLaunchStatusResponse(request_id, sender_id);
  EXPECT_TRUE(peer_socket().Send(allowed_response).ok());
  EXPECT_FALSE(ran);

  CastMessage receiver_status_response =
      CreateReceiverStatusResponse(request_id, sender_id, "session1");
  EXPECT_TRUE(peer_socket().Send(receiver_status_response).ok());
  EXPECT_TRUE(ran);
}

// A LAUNCH_ERROR with a recognized `extendedError` should have message.
TEST_F(CastPlatformClientTest, LaunchSessionExtendedErrorUserNotAllowed) {
  int request_id = -1;
  std::string sender_id;
  ExpectLaunchRequest("AAAAAAAA", &request_id, &sender_id);
  bool ran = false;
  LaunchSession("receiverId1", "AAAAAAAA",
                [&ran](ErrorOr<CastPlatformClient::ReceiverStatus> result) {
                  ASSERT_TRUE(result.is_error());
                  EXPECT_EQ(result.error().message(),
                            kMessageValueUserNotAllowed);
                  ran = true;
                });

  CastMessage launch_response = CreateLaunchExtendedErrorResponse(
      request_id, sender_id, kMessageValueUserNotAllowed);
  EXPECT_TRUE(peer_socket().Send(launch_response).ok());
  EXPECT_TRUE(ran);
}

TEST_F(CastPlatformClientTest, LaunchSessionUnknownReceiver) {
  bool ran = false;
  std::optional<int> maybe_request_id =
      LaunchSession("not-a-real-receiver", "AAAAAAAA",
                    [&ran](ErrorOr<CastPlatformClient::ReceiverStatus> result) {
                      EXPECT_TRUE(result.is_error());
                      ran = true;
                    });
  EXPECT_FALSE(maybe_request_id);
  EXPECT_TRUE(ran);
}

TEST_F(CastPlatformClientTest, StopSessionError) {
  int request_id = -1;
  std::string sender_id;
  ExpectStopRequest("session1", &request_id, &sender_id);
  bool ran = false;
  platform_client_.StopSession("receiverId1", "session1",
                               [&ran](const Error& error) {
                                 EXPECT_FALSE(error.ok());
                                 ran = true;
                               });

  CastMessage stop_response =
      CreateStopErrorResponse(request_id, sender_id, "INVALID_SESSION_ID");
  EXPECT_TRUE(peer_socket().Send(stop_response).ok());
  EXPECT_TRUE(ran);
}

// A successful STOP with a follow-up RECEIVER_STATUS that no longer lists the
// stopped session, rather than with a dedicated response type.
TEST_F(CastPlatformClientTest, StopSessionSuccess) {
  int request_id = -1;
  std::string sender_id;
  ExpectStopRequest("session1", &request_id, &sender_id);
  bool ran = false;
  platform_client_.StopSession("receiverId1", "session1",
                               [&ran](const Error& error) {
                                 EXPECT_TRUE(error.ok());
                                 ran = true;
                               });

  // No applications running any more -- "session1" is gone.
  CastMessage status_response =
      CreateReceiverStatusResponse(request_id, sender_id, "");
  EXPECT_TRUE(peer_socket().Send(status_response).ok());
  EXPECT_TRUE(ran);
}

// A RECEIVER_STATUS that still lists the target session as running doesn't
// confirm the stop -- the request should keep waiting rather than resolve.
TEST_F(CastPlatformClientTest, StopSessionStillRunningKeepsWaiting) {
  int request_id = -1;
  std::string sender_id;
  ExpectStopRequest("session1", &request_id, &sender_id);
  bool ran = false;
  platform_client_.StopSession("receiverId1", "session1",
                               [&ran](const Error& error) { ran = true; });

  CastMessage status_response =
      CreateReceiverStatusResponse(request_id, sender_id, "session1");
  EXPECT_TRUE(peer_socket().Send(status_response).ok());
  EXPECT_FALSE(ran);

  // Eventually times out rather than hanging forever.
  clock_.Advance(std::chrono::seconds(61));
  EXPECT_TRUE(ran);
}

// Silence does not imply success: if neither a confirming RECEIVER_STATUS
// nor a rejection arrives before the timeout, StopSession() reports failure.
TEST_F(CastPlatformClientTest, StopSessionTimeoutIsFailure) {
  int request_id = -1;
  std::string sender_id;
  ExpectStopRequest("session1", &request_id, &sender_id);
  bool ran = false;
  platform_client_.StopSession("receiverId1", "session1",
                               [&ran](const Error& error) {
                                 EXPECT_FALSE(error.ok());
                                 ran = true;
                               });
  EXPECT_FALSE(ran);

  clock_.Advance(std::chrono::seconds(61));
  EXPECT_TRUE(ran);
}

}  // namespace openscreen::cast
