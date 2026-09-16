// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/impl/quiche_web_transport_session.h"

#include <utility>

#include "platform/api/task_runner.h"
#include "platform/impl/quiche_web_transport_stream.h"
#include "quiche/quic/core/http/web_transport_http3.h"
#include "util/osp_logging.h"

namespace openscreen {

class QuicheWebTransportSession::SessionVisitorProxy
    : public webtransport::SessionVisitor {
 public:
  explicit SessionVisitorProxy(QuicheWebTransportSession* session)
      : session_(session) {}
  ~SessionVisitorProxy() override {
    if (session_) {
      session_->OnProxyDestroyed();
    }
  }

  void ClearSession() { session_ = nullptr; }

  void OnSessionReady() override {
    if (session_) {
      session_->OnSessionReady();
    }
  }
  void OnSessionClosed(webtransport::SessionErrorCode error_code,
                       const std::string& error_message) override {
    if (session_) {
      session_->OnSessionClosed(error_code, error_message);
    }
  }
  void OnIncomingBidirectionalStreamAvailable() override {
    if (session_) {
      session_->OnIncomingBidirectionalStreamAvailable();
    }
  }
  void OnIncomingUnidirectionalStreamAvailable() override {}
  void OnDatagramReceived(absl::string_view datagram) override {}
  void OnCanCreateNewOutgoingBidirectionalStream() override {}
  void OnCanCreateNewOutgoingUnidirectionalStream() override {}

 private:
  raw_ptr<QuicheWebTransportSession> session_;
};

QuicheWebTransportSession::QuicheWebTransportSession(
    webtransport::Session* session,
    TaskRunner& task_runner)
    : session_(session), task_runner_(task_runner) {
  OSP_CHECK(session_);
  auto proxy = std::make_unique<SessionVisitorProxy>(this);
  proxy_ = proxy.get();
  auto* http3_session = static_cast<quic::WebTransportHttp3*>(session_.get());
  http3_session->SetVisitor(std::move(proxy));
}

QuicheWebTransportSession::QuicheWebTransportSession(
    webtransport::Session* session,
    TaskRunner& task_runner,
    std::unique_ptr<webtransport::SessionVisitor>& visitor_out)
    : session_(session), task_runner_(task_runner) {
  OSP_CHECK(session_);
  auto proxy = std::make_unique<SessionVisitorProxy>(this);
  proxy_ = proxy.get();
  visitor_out = std::move(proxy);
}

QuicheWebTransportSession::~QuicheWebTransportSession() {
  OSP_CHECK(task_runner_->IsRunningOnTaskRunner());
  for (auto& stream : outgoing_streams_) {
    stream->OnSessionDestroyed();
  }
  for (const auto& stream : incoming_streams_) {
    stream->OnSessionDestroyed();
  }
  if (proxy_) {
    proxy_->ClearSession();
  }
}

void QuicheWebTransportSession::SetDelegate(Delegate* delegate) {
  delegate_ = delegate;
  if (delegate_ && ready_) {
    OSP_LOG_INFO << "SetDelegate: Session already ready, calling delegate "
                    "OnSessionReady immediately.";
    WeakPtr<QuicheWebTransportSession> weak_this = weak_factory_.GetWeakPtr();
    delegate_->OnSessionReady(this);
    if (weak_this) {
      OnIncomingBidirectionalStreamAvailable();
    }
  }
}

ErrorOr<std::unique_ptr<WebTransportStream>>
QuicheWebTransportSession::CreateOutgoingStream() {
  if (!session_) {
    return Error(Error::Code::kConnectionFailed, "Session closed");
  }
  webtransport::Stream* stream = session_->OpenOutgoingBidirectionalStream();
  if (!stream) {
    return Error(Error::Code::kConnectionFailed, "Failed to open stream");
  }
  auto stream_impl = std::make_unique<QuicheWebTransportStream>(
      stream, this, QuicheWebTransportStream::Type::kOutgoing);
  RegisterOutgoingStream(stream_impl.get());
  std::unique_ptr<WebTransportStream> base_stream = std::move(stream_impl);
  return base_stream;
}

void QuicheWebTransportSession::Close(const Error& error) {
  if (session_) {
    session_->CloseSession(static_cast<uint32_t>(error.code()),
                           error.message());
  }
}

void QuicheWebTransportSession::OnIncomingBidirectionalStreamAvailable() {
  WeakPtr<QuicheWebTransportSession> weak_this = weak_factory_.GetWeakPtr();
  while (weak_this && delegate_ && session_) {
    webtransport::Stream* stream =
        session_->AcceptIncomingBidirectionalStream();
    if (!stream) {
      break;
    }
    auto stream_impl = std::make_unique<QuicheWebTransportStream>(
        stream, this, QuicheWebTransportStream::Type::kIncoming);
    auto* stream_ptr = stream_impl.get();
    incoming_streams_.push_back(std::move(stream_impl));
    delegate_->OnIncomingStream(stream_ptr);
  }
}

void QuicheWebTransportSession::OnSessionReady() {
  OSP_LOG_INFO << "QuicheWebTransportSession::OnSessionReady called. delegate="
               << delegate_;
  ready_ = true;
  if (delegate_) {
    delegate_->OnSessionReady(this);
  }
}

void QuicheWebTransportSession::OnSessionClosed(
    webtransport::SessionErrorCode error_code,
    std::string_view error_message) {
  OSP_LOG_INFO
      << "QuicheWebTransportSession::OnSessionClosed called. error_code="
      << error_code << ", error_message=\"" << error_message << "\"";
  if (delegate_) {
    delegate_->OnSessionClosed(this, Error(static_cast<Error::Code>(error_code),
                                           std::string(error_message)));
  }
}

void QuicheWebTransportSession::OnProxyDestroyed() {
  proxy_ = nullptr;
  session_ = nullptr;
}

void QuicheWebTransportSession::RegisterOutgoingStream(
    QuicheWebTransportStream* stream) {
  outgoing_streams_.push_back(stream);
}

void QuicheWebTransportSession::UnregisterStream(
    QuicheWebTransportStream* stream) {
  for (auto it = outgoing_streams_.begin(); it != outgoing_streams_.end();
       ++it) {
    if (*it == stream) {
      outgoing_streams_.erase(it);
      break;
    }
  }
}

void QuicheWebTransportSession::RemoveIncomingStream(
    QuicheWebTransportStream* stream) {
  for (auto it = incoming_streams_.begin(); it != incoming_streams_.end();
       ++it) {
    if (it->get() == stream) {
      auto owned = std::move(*it);
      incoming_streams_.erase(it);
      break;
    }
  }
}

}  // namespace openscreen
