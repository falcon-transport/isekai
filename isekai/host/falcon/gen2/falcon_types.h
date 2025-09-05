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

#ifndef ISEKAI_HOST_FALCON_GEN2_FALCON_TYPES_H_
#define ISEKAI_HOST_FALCON_GEN2_FALCON_TYPES_H_

#include <cstdint>
#include <utility>
#include <vector>

#include "absl/time/time.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/rue/bits.h"

namespace isekai {

// Holds the connection level metadata.
struct Gen2ConnectionMetadata : ConnectionMetadata {
  Gen2ConnectionMetadata() {}
  uint32_t degree_of_multipathing = 1;
};

struct Gen2CongestionControlMetadata : CongestionControlMetadata {
  Gen2CongestionControlMetadata() {}
  // These fields are Gen2-specific.
  //
  uint32_t gen2_plb_state
      : falcon_rue::kPlbStateBits;  // PLB state in Gen2 has a separate field in
                                    // the datapath
  //
  // bit sizing as commented.
  std::vector<uint8_t> gen2_flow_weights;  // Each weights is 4 bits.
  std::vector<uint32_t> gen2_flow_labels;  // Each label is 20 bits.
  // Number of packets ACKed since last successful RUE event for every flow.
  std::vector<uint32_t> gen2_num_acked;
  // Set when posting RUE event for a flow, reset when the corresponding
  // RUE response is processed.
  std::vector<bool> gen2_outstanding_rue_event;
  // Record of the time of the last successful RUE event for every flow.
  std::vector<absl::Duration> gen2_last_rue_event_time;
};

struct Gen2TransactionMetadata : TransactionMetadata {
  // Represents if this transaction should decrement ORRC on receiving Pull
  // Response. Along with this flag, the transaction type as well as the global
  // config flag which decides if ORRC/ORC decrement happens on Pull Response or
  // Pull Request also determines how ORRC/ORC mutation happens. This additional
  // flag is required to handle the case where a retransmitted Pull Request is
  // in the retransmission scheduler and we get a Pull Response, in that case we
  // should not decrement ORRC since its decremented already when the Pull
  // Request is put in the retx scheduler.
  Gen2TransactionMetadata() {}
  Gen2TransactionMetadata(uint32_t rsn_arg, TransactionType ctype,
                          TransactionLocation location_arg)
      : TransactionMetadata(rsn_arg, ctype, location_arg) {}
  bool decrement_orrc_on_pull_response = true;
};

// Indicates the location of the packet buffers.
enum class PacketBufferLocation {
  kSram,
};

struct Gen2ReceivedPacketContext : ReceivedPacketContext {
  Gen2ReceivedPacketContext() {}
  // Location of the packet buffer, by default packets received are in SRAM.
  PacketBufferLocation location = PacketBufferLocation::kSram;
};

struct Gen2OutstandingPacketContext : OutstandingPacketContext {
  // For multipath connections, flow_id represents the flow ID that the
  // outstanding packet was transmitted on. This field is used for ACK unrolling
  // in Gen2.
  uint8_t flow_id = 0;

  Gen2OutstandingPacketContext() {}
  Gen2OutstandingPacketContext(uint32_t rsn_arg, falcon::PacketType type_arg,
                               uint8_t flow_id)
      : OutstandingPacketContext(rsn_arg, type_arg), flow_id(flow_id) {}
};

struct Gen2AckCoalescingKey : AckCoalescingKey {
  // For multipath connections, flow_id represents the flow ID that the
  // outstanding packet was transmitted on. This field is used to enable ACK
  // coalescing to happen at the <connection, flow> pair in Gen2.
  uint8_t flow_id = 0;

  Gen2AckCoalescingKey(uint32_t scid_arg, uint8_t flow_id)
      : AckCoalescingKey(scid_arg), flow_id(flow_id) {}
  Gen2AckCoalescingKey() {}
  template <typename H>
  friend H AbslHashValue(H state, const Gen2AckCoalescingKey& value) {
    state = H::combine(std::move(state), value.scid, value.flow_id);
    return state;
  }
  // Two variables are equal if they share the same scid and flow_id.
  inline bool operator==(const Gen2AckCoalescingKey& other) const {
    return other.scid == scid && other.flow_id == flow_id;
  }
};
typedef Gen2AckCoalescingKey Gen2RueKey;

// These values are used by the backpressure manager.
struct ConnectionRdmaXoffMetadata {
  // If the connection is RC, this metadata stores the corresponding qp_id of
  // this connection.
  uint64_t qp_id = 0;
  // Current request/response xoff status asserted by the backpressure manager.
  UlpXoff ulp_xoff;
  // Dynamic alpha values calculated by the RUE.
  uint8_t alpha_request = 0;
  uint8_t alpha_response = 0;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN2_FALCON_TYPES_H_
