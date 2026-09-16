// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PLATFORM_IMPL_QUICHE_WEB_TRANSPORT_SESSION_H_
#define PLATFORM_IMPL_QUICHE_WEB_TRANSPORT_SESSION_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "platform/api/web_transport.h"
#include "quiche/web_transport/web_transport.h"
#include "util/raw_ptr.h"
#include "util/raw_ref.h"
#include "util/weak_ptr.h"

namespace openscreen {

// QUICHE-based implementation of WebTransportSession. Wraps a raw
// webtransport::Session and adapts events (incoming streams, session
// ready/closed) using a visitor proxy.
class QuicheWebTransportStream;

class QuicheWebTransportSession : public WebTransportSession {
 public:
  QuicheWebTransportSession(webtransport::Session* session,
                            TaskRunner& task_runner);
  QuicheWebTransportSession(
      webtransport::Session* session,
      TaskRunner& task_runner,
      std::unique_ptr<webtransport::SessionVisitor>& visitor_out);
  ~QuicheWebTransportSession() override;

  QuicheWebTransportSession(const QuicheWebTransportSession&) = delete;
  QuicheWebTransportSession& operator=(const QuicheWebTransportSession&) =
      delete;
  QuicheWebTransportSession(QuicheWebTransportSession&&) noexcept = delete;
  QuicheWebTransportSession& operator=(QuicheWebTransportSession&&) noexcept =
      delete;

  // WebTransportSession implementation.
  void SetDelegate(Delegate* delegate) override;
  ErrorOr<std::unique_ptr<WebTransportStream>> CreateOutgoingStream() override;
  void Close(const Error& error) override;

  // Callbacks from webtransport::SessionVisitor.
  void OnIncomingBidirectionalStreamAvailable();
  void OnSessionReady();
  void OnSessionClosed(webtransport::SessionErrorCode error_code,
                       std::string_view error_message);

  void RegisterOutgoingStream(QuicheWebTransportStream* stream);
  void UnregisterStream(QuicheWebTransportStream* stream);
  void RemoveIncomingStream(QuicheWebTransportStream* stream);

 private:
  class SessionVisitorProxy;

  void OnProxyDestroyed();

  raw_ptr<webtransport::Session> session_;
  raw_ptr<Delegate> delegate_;
  raw_ptr<SessionVisitorProxy> proxy_;
  const raw_ref<TaskRunner> task_runner_;
  bool ready_ = false;

  std::vector<std::unique_ptr<QuicheWebTransportStream>> incoming_streams_;
  std::vector<raw_ptr<QuicheWebTransportStream>> outgoing_streams_;

  WeakPtrFactory<QuicheWebTransportSession> weak_factory_{this};
};

}  // namespace openscreen

#endif  // PLATFORM_IMPL_QUICHE_WEB_TRANSPORT_SESSION_H_
