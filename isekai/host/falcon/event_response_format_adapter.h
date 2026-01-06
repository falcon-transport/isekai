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

#ifndef ISEKAI_HOST_FALCON_EVENT_RESPONSE_FORMAT_ADAPTER_H_
#define ISEKAI_HOST_FALCON_EVENT_RESPONSE_FORMAT_ADAPTER_H_

#include <cstdint>
#include <memory>

#include "isekai/common/config.pb.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/rue/algorithm/swift.pb.h"
#include "isekai/host/falcon/rue/format_gen2.h"
#include "isekai/host/falcon/rue/format_gen3.h"

namespace isekai {

// This class is responsible for handling format changes in the RUE event and
// response formats. Any function in the RUE module that access fields in the
// response or event that are not shared across event/response formats should be
// managed by this EventResponseFormatAdapter class (e.g., functions filling the
// events to be sent to algorithm, the function which handles the RUE
// response from the algorithm, or functions which return the RUE adapters for
// different algorithms).
template <typename EventT, typename ResponseT>
class EventResponseFormatAdapter {
 public:
  explicit EventResponseFormatAdapter(const FalconModelInterface* falcon) {
    falcon_ = falcon;
  }

  // Changes the connection state as a result of a received RUE response.
  void UpdateConnectionStateFromResponse(ConnectionState* connection_state,
                                         const ResponseT* response) const;

  // Returns true if the response is signaling the datapath to repath the
  // connection.
  bool IsRandomizePath(const ResponseT* response) const;

  // Fills an RTO event before it can be sent to the algorithm.
  void FillTimeoutRetransmittedEvent(EventT& event, const RueKey* rue_key,
                                     const Packet* packet,
                                     const CongestionControlMetadata& ccmeta,
                                     uint8_t retransmit_count) const;

  void FillNackEvent(EventT& event, const RueKey* rue_key, const Packet* packet,
                     const CongestionControlMetadata& ccmeta,
                     uint32_t num_packets_acked) const;

  void FillExplicitAckEvent(EventT& event, const RueKey* rue_key,
                            const Packet* packet,
                            const CongestionControlMetadata& ccmeta,
                            uint32_t num_packets_acked, bool eack,
                            bool eack_drop) const;

 private:
  const FalconModelInterface* falcon_;
};

// Gen_2 template specializations.
template <>
void EventResponseFormatAdapter<falcon_rue::Event_Gen2,
                                falcon_rue::Response_Gen2>::
    UpdateConnectionStateFromResponse(
        ConnectionState* connection_state,
        const falcon_rue::Response_Gen2* response) const;

template <>
bool EventResponseFormatAdapter<falcon_rue::Event_Gen2,
                                falcon_rue::Response_Gen2>::
    IsRandomizePath(const falcon_rue::Response_Gen2* response) const;

template <>
void EventResponseFormatAdapter<falcon_rue::Event_Gen2,
                                falcon_rue::Response_Gen2>::
    FillTimeoutRetransmittedEvent(falcon_rue::Event_Gen2& event,
                                  const RueKey* rue_key, const Packet* packet,
                                  const CongestionControlMetadata& ccmeta,
                                  uint8_t retransmit_count) const;

template <>
void EventResponseFormatAdapter<falcon_rue::Event_Gen2,
                                falcon_rue::Response_Gen2>::
    FillNackEvent(falcon_rue::Event_Gen2& event, const RueKey* rue_key,
                  const Packet* packet, const CongestionControlMetadata& ccmeta,
                  uint32_t num_packets_acked) const;

template <>
void EventResponseFormatAdapter<falcon_rue::Event_Gen2,
                                falcon_rue::Response_Gen2>::
    FillExplicitAckEvent(falcon_rue::Event_Gen2& event, const RueKey* rue_key,
                         const Packet* packet,
                         const CongestionControlMetadata& ccmeta,
                         uint32_t num_packets_acked, bool eack,
                         bool eack_drop) const;

// Gen_3 template specializations.
template <>
void EventResponseFormatAdapter<falcon_rue::EVENT_Gen3,
                                falcon_rue::Response_Gen3>::
    UpdateConnectionStateFromResponse(
        ConnectionState* connection_state,
        const falcon_rue::Response_Gen3* response) const;

template <>
bool EventResponseFormatAdapter<falcon_rue::EVENT_Gen3,
                                falcon_rue::Response_Gen3>::
    IsRandomizePath(const falcon_rue::Response_Gen3* response) const;

template <>
void EventResponseFormatAdapter<falcon_rue::EVENT_Gen3,
                                falcon_rue::Response_Gen3>::
    FillTimeoutRetransmittedEvent(falcon_rue::EVENT_Gen3& event,
                                  const RueKey* rue_key, const Packet* packet,
                                  const CongestionControlMetadata& ccmeta,
                                  uint8_t retransmit_count) const;

template <>
void EventResponseFormatAdapter<falcon_rue::EVENT_Gen3,
                                falcon_rue::Response_Gen3>::
    FillNackEvent(falcon_rue::EVENT_Gen3& event, const RueKey* rue_key,
                  const Packet* packet, const CongestionControlMetadata& ccmeta,
                  uint32_t num_packets_acked) const;

template <>
void EventResponseFormatAdapter<falcon_rue::EVENT_Gen3,
                                falcon_rue::Response_Gen3>::
    FillExplicitAckEvent(falcon_rue::EVENT_Gen3& event, const RueKey* rue_key,
                         const Packet* packet,
                         const CongestionControlMetadata& ccmeta,
                         uint32_t num_packets_acked, bool eack,
                         bool eack_drop) const;

//
// TLP

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_EVENT_RESPONSE_FORMAT_ADAPTER_H_
