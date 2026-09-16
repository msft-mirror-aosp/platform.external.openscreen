// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/impl/quiche_web_transport_stream.h"

#include <array>
#include <format>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/types/span.h"
#include "platform/base/error.h"
#include "platform/impl/quiche_web_transport_session.h"
#include "util/osp_logging.h"

namespace openscreen {

class QuicheWebTransportStream::StreamVisitorProxy
    : public webtransport::StreamVisitor {
 public:
  explicit StreamVisitorProxy(QuicheWebTransportStream* stream)
      : stream_(stream) {}
  ~StreamVisitorProxy() override {
    if (stream_) {
      stream_->OnProxyDestroyed();
    }
  }

  void ClearStream() { stream_ = nullptr; }

  void OnCanRead() override {
    if (stream_) {
      stream_->OnCanRead();
    }
  }
  void OnCanWrite() override {
    if (stream_) {
      stream_->OnCanWrite();
    }
  }
  void OnResetStreamReceived(webtransport::StreamErrorCode error) override {
    if (stream_) {
      stream_->OnResetStreamReceived(error);
    }
  }
  void OnStopSendingReceived(webtransport::StreamErrorCode error) override {
    if (stream_) {
      stream_->OnStopSendingReceived(error);
    }
  }
  void OnWriteSideInDataRecvdState() override {}

 private:
  raw_ptr<QuicheWebTransportStream> stream_;
};

QuicheWebTransportStream::QuicheWebTransportStream(
    webtransport::Stream* stream,
    QuicheWebTransportSession* session,
    Type type)
    : stream_(stream), session_(session), type_(type) {
  auto proxy = std::make_unique<StreamVisitorProxy>(this);
  proxy_ = proxy.get();
  stream_->SetVisitor(std::move(proxy));
}

QuicheWebTransportStream::~QuicheWebTransportStream() {
  // Notify the delegate first, so that a non-owning delegate can drop its
  // pointer to `this` before anything is torn down. `delegate_` is cleared
  // beforehand so that the notification cannot trigger a re-entrant callback
  // into a partially destroyed object.
  if (Delegate* delegate = delegate_) {
    delegate_ = nullptr;
    delegate->OnDestroyed(this);
  }
  if (session_) {
    session_->UnregisterStream(this);
    session_ = nullptr;
  }
  if (proxy_) {
    proxy_->ClearStream();
  }
  if (stream_) {
    stream_->SetVisitor(nullptr);
  }
}

void QuicheWebTransportStream::SetDelegate(Delegate* delegate) {
  delegate_ = delegate;
  if (delegate_) {
    // Trigger read in case there is already data available.
    OnCanRead();
  }
}

bool QuicheWebTransportStream::Write(ByteView data) {
  if (!stream_ || !stream_->CanWrite()) {
    return false;
  }
  if (data.empty()) {
    return true;
  }
  return stream_->Write(std::string_view(
      reinterpret_cast<const char*>(data.data()), data.size()));
}

void QuicheWebTransportStream::Close() {
  if (stream_) {
    if (!stream_->SendFin()) {
      OSP_LOG_ERROR << "Failed to send FIN on stream";
    }
  }
}

void QuicheWebTransportStream::OnCanRead() {
  if (!delegate_ || !stream_) {
    return;
  }

  // Monitor if 'this' is deleted synchronously during delegate callback
  // invocations.
  auto weak_this = weak_factory_.GetWeakPtr();

  // Stack-allocate to prevent heap allocation overhead in the I/O loop.
  constexpr size_t kBufferSize = 4096;
  std::array<uint8_t, kBufferSize> buffer;

  while (weak_this) {
    auto [bytes_read, fin] = stream_->Read(
        absl::MakeSpan(reinterpret_cast<char*>(buffer.data()), buffer.size()));

    if (bytes_read > 0) {
      // OnRead can synchronously destroy 'this'.
      delegate_->OnRead(this, ByteView(buffer.data(), bytes_read));
    }

    // Safety: Stop immediately if OnRead deleted this object.
    if (!weak_this) {
      return;
    }

    if (fin) {
      // OnClose can also synchronously destroy 'this'.
      delegate_->OnClose(this);
      return;
    }

    // If no bytes were read and there was no FIN, the socket buffer is drained.
    if (bytes_read == 0) {
      break;
    }
  }
}

void QuicheWebTransportStream::OnCanWrite() {
  if (delegate_) {
    delegate_->OnWriteReady(this);
  }
}

void QuicheWebTransportStream::OnResetStreamReceived(
    webtransport::StreamErrorCode error) {
  OnConnectionFailed(error, "Stream reset by peer");
}

void QuicheWebTransportStream::OnStopSendingReceived(
    webtransport::StreamErrorCode error) {
  OnConnectionFailed(error, "Stop sending received");
}

void QuicheWebTransportStream::OnConnectionFailed(
    webtransport::StreamErrorCode error_code,
    std::string_view message) {
  auto weak_this = weak_factory_.GetWeakPtr();
  if (delegate_) {
    delegate_->OnError(
        this, Error(Error::Code::kConnectionFailed,
                    std::format("{}. webtransport error code: {}", message,
                                error_code)));
  }

  // Invoking the OnError() callback may have deleted `this`.
  if (!weak_this) {
    return;
  }

  if (session_ && type_ == Type::kIncoming) {
    session_->RemoveIncomingStream(this);
  }
}

void QuicheWebTransportStream::OnProxyDestroyed() {
  proxy_ = nullptr;
  stream_ = nullptr;
  if (session_) {
    if (type_ == Type::kIncoming) {
      session_->RemoveIncomingStream(this);
      return;
    }
    session_->UnregisterStream(this);
    session_ = nullptr;
  }
}

void QuicheWebTransportStream::OnSessionDestroyed() {
  session_ = nullptr;
  stream_ = nullptr;
}

}  // namespace openscreen
