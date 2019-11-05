// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DISCOVERY_DNSSD_IMPL_QUERIER_IMPL_H_
#define DISCOVERY_DNSSD_IMPL_QUERIER_IMPL_H_

#include <unordered_map>

#include "absl/hash/hash.h"
#include "absl/strings/string_view.h"
#include "cast/common/mdns/mdns_record_changed_callback.h"
#include "cast/common/mdns/mdns_records.h"
#include "cast/common/mdns/public/mdns_service.h"
#include "discovery/dnssd/impl/constants.h"
#include "discovery/dnssd/impl/dns_data.h"
#include "discovery/dnssd/public/instance_record.h"
#include "discovery/dnssd/public/querier.h"

namespace openscreen {
namespace discovery {

class QuerierImpl : public DnsSdQuerier,
                    public cast::mdns::MdnsRecordChangedCallback {
 public:
  // |querier| must outlive the QuerierImpl instance constructed.
  explicit QuerierImpl(cast::mdns::MdnsService* querier);
  virtual ~QuerierImpl() override = default;

  inline bool IsQueryRunning(const absl::string_view& service,
                             const absl::string_view& domain) const;

  // DnsSdQuerier overrides.
  void StartQuery(const absl::string_view& service,
                  const absl::string_view& domain,
                  Callback* callback) override;
  void StopQuery(const absl::string_view& service,
                 const absl::string_view& domain,
                 Callback* callback) override;

  // cast::mdns::MdnsRecordChangedCallback overrides.
  void OnRecordChanged(const cast::mdns::MdnsRecord& record,
                       cast::mdns::RecordChangedEvent event) override;

 private:
  // Map from {instance, service, domain} to the data received so far for this
  // service instance.
  std::unordered_map<InstanceKey, DnsData, absl::Hash<InstanceKey>>
      received_records_;

  // Stores the association between {service, domain} and Callback to call for
  // records with this ServiceKey.
  std::map<ServiceKey, std::vector<Callback*>> callback_map_;

  cast::mdns::MdnsService* const mdns_querier_;
};

}  // namespace discovery
}  // namespace openscreen

#endif  // DISCOVERY_DNSSD_IMPL_QUERIER_IMPL_H_