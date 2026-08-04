// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PLATFORM_IMPL_QUIC_QUIC_ALARM_FACTORY_IMPL_H_
#define PLATFORM_IMPL_QUIC_QUIC_ALARM_FACTORY_IMPL_H_

#include "platform/api/task_runner.h"
#include "quiche/quic/core/quic_alarm.h"
#include "quiche/quic/core/quic_alarm_factory.h"
#include "quiche/quic/core/quic_clock.h"
#include "util/raw_ptr.h"
#include "util/raw_ref.h"

namespace openscreen {

class QuicAlarmFactoryImpl : public quic::QuicAlarmFactory {
 public:
  QuicAlarmFactoryImpl(TaskRunner& task_runner, const quic::QuicClock* clock);
  QuicAlarmFactoryImpl(const QuicAlarmFactoryImpl&) = delete;
  QuicAlarmFactoryImpl& operator=(const QuicAlarmFactoryImpl&) = delete;
  QuicAlarmFactoryImpl(QuicAlarmFactoryImpl&&) noexcept = delete;
  QuicAlarmFactoryImpl& operator=(QuicAlarmFactoryImpl&&) noexcept = delete;
  ~QuicAlarmFactoryImpl() override;

  // quic::QuicAlarmFactory overrides.
  quic::QuicAlarm* CreateAlarm(quic::QuicAlarm::Delegate* delegate) override;
  quic::QuicArenaScopedPtr<quic::QuicAlarm> CreateAlarm(
      quic::QuicArenaScopedPtr<quic::QuicAlarm::Delegate> delegate,
      quic::QuicConnectionArena* arena) override;

 private:
  const raw_ref<TaskRunner> task_runner_;
  raw_ptr<const quic::QuicClock> clock_;
};

}  // namespace openscreen

#endif  // PLATFORM_IMPL_QUIC_QUIC_ALARM_FACTORY_IMPL_H_
