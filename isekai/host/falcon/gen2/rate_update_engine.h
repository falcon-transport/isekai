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

#ifndef ISEKAI_HOST_FALCON_GEN2_RATE_UPDATE_ENGINE_H_
#define ISEKAI_HOST_FALCON_GEN2_RATE_UPDATE_ENGINE_H_

#include <cstdint>
#include <memory>

#include "absl/time/time.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/rate_update_engine.h"
#include "isekai/host/falcon/rue/algorithm/swift.h"
#include "isekai/host/falcon/rue/format_gen2.h"

namespace isekai {

class Gen2RateUpdateEngine : public ProtocolRateUpdateEngine {
 public:
  explicit Gen2RateUpdateEngine(FalconModelInterface* falcon);

  void PacketTimeoutRetransmitted(uint32_t cid, const Packet* packet,
                                  uint8_t retransmit_count) override;

  // Initializes the Gen2-specific fields in the connection's
  // CongestionControlMetadata.
  void InitializeGenSpecificMetadata(
      CongestionControlMetadata& metadata) override;

 private:
  // Encodes the bit representation of the flow ID in the least significant
  // bits of the flow label. The number of the least significant reserved for
  // the flow ID depends on the num_flows for the connection. This function does
  // assume that num_flows is a power of 2.
  uint32_t EncodeFlowIdBitsInFlowLabel(uint32_t flow_label, uint8_t flow_id,
                                       uint32_t num_flows);
  // Returns the last RUE event time for the input RueKey.
  absl::Duration GetLastEventTime(const RueKey* rue_key) const override;
  // Updates last RUE event time for the input RueKey to the current simulation
  // time .
  void UpdateLastEventTime(const RueKey* rue_key) const override;
  // Returns whether there is an outstanding event for the input RueKey.
  bool GetOutstandingEvent(const RueKey* rue_key) const override;
  // Updates the outstanding event state for the input RueKey to the input
  // value. Returns true if the previous value was different than the new input
  // value, false otherwise.
  bool UpdateOutstandingEvent(const RueKey* rue_key, bool value) const override;
  // Returns the num_acked value since the last successful RUE event for the
  // input RueKey.
  uint32_t GetNumAcked(const RueKey* rue_key) const override;
  // Resets the num_acked value for the input RueKey to 0.
  void ResetNumAcked(const RueKey* rue_key) const override;
  // Initializes the delay_state in CongestionControlMetadata.
  void InitializeDelayState(CongestionControlMetadata& metadata) const override;

  // Handles logging flow-level stats (e.g., network delays) from the ACK packet
  // received.
  void CollectAckStats(const RueKey* rue_key, const Packet* packet) override;
  // Handles logging the flow-level num_acked stat for a RUE event by an
  // ACK/NACK before that event is enqueued to be sent to Swift.
  void CollectNumAckedStats(const RueKey* rue_key,
                            uint32_t num_packets_acked) override;
  // Handles logging the flow-level CC metrics that are output by a RUE response
  // and applied to the datapath (e.g., flow weights, flow-level repathing).
  void CollectCongestionControlMetricsAfterResponse(
      const RueKey* rue_key, const CongestionControlMetadata& metadata,
      const ResponseMetadata& response_metadata) override;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN2_RATE_UPDATE_ENGINE_H_
