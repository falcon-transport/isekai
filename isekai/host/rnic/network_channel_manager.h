/*
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ISEKAI_HOST_RNIC_NETWORK_CHANNEL_MANAGER_H_
#define ISEKAI_HOST_RNIC_NETWORK_CHANNEL_MANAGER_H_

#include <stddef.h>
#include <sys/types.h>

#include <queue>
#include <vector>

#include "absl/time/time.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"
#include "omnetpp/cmessage.h"
#include "omnetpp/csimplemodule.h"
#include "omnetpp/csimulation.h"

// Undef the macros in mac_address.h that conflict with the definitions in INET.
#undef ETH_ALEN
#undef ETHER_TYPE_LEN
#include "inet/linklayer/ethernet/EtherPhyFrame_m.h"

namespace isekai {

// NetworkChannelManager manages a host's network channel and is responsible to
// actually push all packets from a host to the network channel. It also
// enforces any added delays set in the packet's metadata.
class NetworkChannelManager {
 public:
  // The struct that the NetworkChannelManager module uses to manage scheduling
  // packets on the wire.
  struct DelayedEthernetSignal {
    // The actual ethernet packet that will be transmitted on the wire.
    inet::EthernetSignal* signal;
    // The earliest time that this packet can be transmitted on the wire.
    absl::Duration scheduled_time;
    // The transmission delay of this packet keeping the channel busy.
    absl::Duration transmission_delay;
  };
  // DelayedEthernetSignal A is larger than DelayedEthernetSignal B if its
  // scheduled_time is later than that of B's.
  struct DelayedEthernetSignalComparator {
    bool operator()(const DelayedEthernetSignal& a,
                    const DelayedEthernetSignal& b) const {
      return a.scheduled_time > b.scheduled_time;
    }
  };

  NetworkChannelManager(omnetpp::cSimpleModule* host_module, Environment* env);
  ~NetworkChannelManager();

  // Prepares the packet (`signal`) with the specified `transmission_delay` to
  // be sent on the wire at least send_delay after now.
  void ScheduleSend(inet::EthernetSignal* signal, absl::Duration send_delay,
                    absl::Duration transmission_delay);

 private:
  // Handles the logic of dequeuing from the priority queue and sending it on
  // the wire.
  void SendOnWire();

  // The next time instance that the channel is not busy and ready to accept a
  // new packet transmission.
  absl::Duration next_available_time_ = absl::ZeroDuration();
  // The priority queue keeping the scheduled packets in increasing order of
  // their scheduled_time.
  std::priority_queue<DelayedEthernetSignal, std::vector<DelayedEthernetSignal>,
                      DelayedEthernetSignalComparator>
      delayed_signals_;
  omnetpp::cSimpleModule* const host_module_;
  Environment* const env_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_RNIC_NETWORK_CHANNEL_MANAGER_H_
