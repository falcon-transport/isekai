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

#include "isekai/host/falcon/gen3/ack_coalescing_engine.h"

#include <memory>

#include "glog/logging.h"
#include "isekai/common/constants.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon_bitmap.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen2/ack_coalescing_engine.h"

namespace isekai {

Gen3AckCoalescingEngine::Gen3AckCoalescingEngine(FalconModelInterface* falcon)
    : Gen2AckCoalescingEngine(falcon),
      rx_window_size_override_(
          falcon->get_config()->gen3_config_options().rx_window_size()) {}

void Gen3AckCoalescingEngine::FillInAckBitmaps(
    Packet* ack_packet, const ConnectionState::ReceiverReliabilityMetadata&
                            rx_reliability_metadata) {
  switch (rx_window_size_override_) {
    case 0:
      // Default to kGen2RxBitmapWidth if value unspecified (fall through).
    case kGen2RxBitmapWidth:
      ack_packet->ack.receiver_request_bitmap =
          dynamic_cast<FalconBitmap<kGen2RxBitmapWidth>&>(
              *(rx_reliability_metadata.request_window_metadata.ack_window));
      ack_packet->ack.receiver_data_bitmap =
          dynamic_cast<FalconBitmap<kGen2RxBitmapWidth>&>(
              *(rx_reliability_metadata.data_window_metadata.ack_window));
      ack_packet->ack.received_bitmap =
          dynamic_cast<FalconBitmap<kGen2RxBitmapWidth>&>(
              *(rx_reliability_metadata.data_window_metadata.receive_window));
      break;
    case kGen3RxBitmapWidth:
      ack_packet->ack.receiver_request_bitmap =
          dynamic_cast<FalconBitmap<kGen3RxBitmapWidth>&>(
              *(rx_reliability_metadata.request_window_metadata.ack_window));
      ack_packet->ack.receiver_data_bitmap =
          dynamic_cast<FalconBitmap<kGen3RxBitmapWidth>&>(
              *(rx_reliability_metadata.data_window_metadata.ack_window));
      ack_packet->ack.received_bitmap =
          dynamic_cast<FalconBitmap<kGen3RxBitmapWidth>&>(
              *(rx_reliability_metadata.data_window_metadata.receive_window));
      break;
    default:
      LOG(FATAL) << "Unsupported RX window size in config: "
                 << rx_window_size_override_;
  }
}

}  // namespace isekai
