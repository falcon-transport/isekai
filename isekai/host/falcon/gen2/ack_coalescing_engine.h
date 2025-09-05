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

#ifndef ISEKAI_HOST_FALCON_GEN2_ACK_COALESCING_ENGINE_H_
#define ISEKAI_HOST_FALCON_GEN2_ACK_COALESCING_ENGINE_H_

#include <cstdint>

#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/ack_coalescing_engine.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen2/falcon_types.h"

namespace isekai {

// Gen2AckCoalescingEngine inherits from AckCoalescingEngine<T>, where the
// template variable T is Gen2AckCoalescingKey.
class Gen2AckCoalescingEngine
    : public AckCoalescingEngine<Gen2AckCoalescingKey> {
 public:
  explicit Gen2AckCoalescingEngine(FalconModelInterface* falcon);
  // Fill in the bitmaps in ACK packets.
  void FillInAckBitmaps(Packet* ack_packet,
                        const ConnectionState::ReceiverReliabilityMetadata&
                            rx_reliability_metadata) override;
  // Generates a Gen2AckCoalescingKey from an incoming packet.
  std::unique_ptr<AckCoalescingKey> GenerateAckCoalescingKeyFromIncomingPacket(
      const Packet* packet) override;
  // Generates a Gen2AckCoalescingKey from an scid value and a Gen2OpaqueCookie
  // reflected back from the ULP.
  std::unique_ptr<AckCoalescingKey> GenerateAckCoalescingKeyFromUlp(
      uint32_t scid, const OpaqueCookie& cookie) override;

 private:
  // Returns the flow label that needs to be used for the current N/ACK.
  uint32_t GetAckFlowLabel(
      const ConnectionState* connection_state,
      const AckCoalescingEntry* ack_coalescing_entry) override;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN2_ACK_COALESCING_ENGINE_H_
