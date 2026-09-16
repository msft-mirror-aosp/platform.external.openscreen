// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PLATFORM_IMPL_QUICHE_WEB_TRANSPORT_STREAM_H_
#define PLATFORM_IMPL_QUICHE_WEB_TRANSPORT_STREAM_H_

#include <string_view>

#include "platform/api/web_transport.h"
#include "quiche/web_transport/web_transport.h"
#include "util/raw_ptr.h"
#include "util/weak_ptr.h"

namespace openscreen {

// QUICHE-based implementation of WebTransportStream. Wraps a raw
// webtransport::Stream and handles stream read/write events using a visitor
// proxy.
class QuicheWebTransportSession;

class QuicheWebTransportStream : public WebTransportStream {
 public:
  enum class Type { kIncoming, kOutgoing };

  QuicheWebTransportStream(webtransport::Stream* stream,
                           QuicheWebTransportSession* session,
                           Type type);
  ~QuicheWebTransportStream() override;

  QuicheWebTransportStream(const QuicheWebTransportStream&) = delete;
  QuicheWebTransportStream& operator=(const QuicheWebTransportStream&) = delete;
  QuicheWebTransportStream(QuicheWebTransportStream&&) noexcept = delete;
  QuicheWebTransportStream& operator=(QuicheWebTransportStream&&) noexcept =
      delete;

  // WebTransportStream implementation.
  void SetDelegate(Delegate* delegate) override;
  bool Write(ByteView data) override;
  void Close() override;

  // Called when the parent session is destroyed.
  void OnSessionDestroyed();

 private:
  class StreamVisitorProxy;

  void OnProxyDestroyed();

  // Callbacks from webtransport::StreamVisitor (via proxy).
  void OnCanRead();
  void OnCanWrite();
  void OnResetStreamReceived(webtransport::StreamErrorCode error);
  void OnStopSendingReceived(webtransport::StreamErrorCode error);
  void OnConnectionFailed(webtransport::StreamErrorCode error_code,
                          std::string_view message);

 private:
  raw_ptr<webtransport::Stream> stream_;
  raw_ptr<QuicheWebTransportSession> session_;
  raw_ptr<Delegate> delegate_;
  raw_ptr<StreamVisitorProxy> proxy_;
  Type type_;
  WeakPtrFactory<QuicheWebTransportStream> weak_factory_{this};
};

}  // namespace openscreen

#endif  // PLATFORM_IMPL_QUICHE_WEB_TRANSPORT_STREAM_H_
