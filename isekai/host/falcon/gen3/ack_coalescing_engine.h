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

#ifndef ISEKAI_HOST_FALCON_GEN3_ACK_COALESCING_ENGINE_H_
#define ISEKAI_HOST_FALCON_GEN3_ACK_COALESCING_ENGINE_H_

#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen2/ack_coalescing_engine.h"

namespace isekai {

class Gen3AckCoalescingEngine : public Gen2AckCoalescingEngine {
 public:
  explicit Gen3AckCoalescingEngine(FalconModelInterface* falcon);

  // Fill in the bitmaps in ACK packets using Gen3 bitmap format (with bitmap
  // size specified in config).
  void FillInAckBitmaps(Packet* ack_packet,
                        const ConnectionState::ReceiverReliabilityMetadata&
                            rx_reliability_metadata) override;

 private:
  // RX window size optionally specified in config; zero if unspecified.
  int rx_window_size_override_ = 0;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN3_ACK_COALESCING_ENGINE_H_
