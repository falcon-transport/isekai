// Copyright 2024 Google LLC

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "isekai/host/falcon/gen2/ack_coalescing_engine.h"

#include <cstdint>
#include <memory>

#include "isekai/common/constants.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon_bitmap.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/ack_coalescing_engine.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"
#include "isekai/host/falcon/gen2/falcon_types.h"
#include "isekai/host/falcon/gen2/falcon_utils.h"

namespace isekai {

Gen2AckCoalescingEngine::Gen2AckCoalescingEngine(FalconModelInterface* falcon)
    : AckCoalescingEngine(falcon) {}

void Gen2AckCoalescingEngine::FillInAckBitmaps(
    Packet* ack_packet, const ConnectionState::ReceiverReliabilityMetadata&
                            rx_reliability_metadata) {
  // FalconBitmap operator= only copies the least significant bits.
  ack_packet->ack.receiver_request_bitmap =
      dynamic_cast<FalconBitmap<kGen2RxBitmapWidth>&>(
          *(rx_reliability_metadata.request_window_metadata.ack_window));
  ack_packet->ack.receiver_data_bitmap =
      dynamic_cast<FalconBitmap<kGen2RxBitmapWidth>&>(
          *(rx_reliability_metadata.data_window_metadata.ack_window));
  ack_packet->ack.received_bitmap =
      dynamic_cast<FalconBitmap<kGen2RxBitmapWidth>&>(
          *(rx_reliability_metadata.data_window_metadata.receive_window));
}

std::unique_ptr<AckCoalescingKey>
Gen2AckCoalescingEngine::GenerateAckCoalescingKeyFromUlp(
    uint32_t scid, const OpaqueCookie& cookie) {
  return std::make_unique<Gen2AckCoalescingKey>(
      scid, dynamic_cast<const Gen2OpaqueCookie&>(cookie).flow_id);
}

std::unique_ptr<AckCoalescingKey>
Gen2AckCoalescingEngine::GenerateAckCoalescingKeyFromIncomingPacket(
    const Packet* packet) {
  uint32_t scid = GetFalconPacketConnectionId(*packet);
  uint8_t flow_id =
      GetFlowIdFromFlowLabel(packet->metadata.flow_label, falcon_, scid);
  return std::make_unique<Gen2AckCoalescingKey>(scid, flow_id);
}

// Returns the flow label that needs to be used for the current N/ACK. For Gen2,
// we reflect in the ACK the flow label of the received data/request packet
// maintained in the ACK coalescing entry.
uint32_t Gen2AckCoalescingEngine::GetAckFlowLabel(
    const ConnectionState* connection_state,
    const AckCoalescingEntry* ack_coalescing_entry) {
  return ack_coalescing_entry->cc_metadata_to_reflect.flow_label;
}

}  // namespace isekai
