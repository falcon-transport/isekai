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

#include "isekai/host/falcon/gen2/connection_state_manager.h"

#include <cstdint>
#include <memory>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "isekai/common/constants.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/connection_state_manager.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen2/connection_state.h"
#include "isekai/host/falcon/gen2/falcon_types.h"
#include "isekai/host/falcon/gen2/falcon_utils.h"

namespace isekai {

Gen2ConnectionStateManager::Gen2ConnectionStateManager(
    FalconModelInterface* falcon)
    : ProtocolConnectionStateManager(falcon) {}

// Creates an ACK coalescing entry for each flow in the connection.
void Gen2ConnectionStateManager::InitializeAckCoalescingEntryForConnection(
    const ConnectionMetadata* connection_metadata) {
  uint32_t multipathing_num_flows =
      GetGen2MultipathingNumFlows(connection_metadata);
  for (uint8_t flow_id = 0; flow_id < multipathing_num_flows; ++flow_id) {
    Gen2AckCoalescingKey ack_coalescing_key(connection_metadata->scid, flow_id);
    falcon_->get_ack_coalescing_engine()->CreateAckCoalescingEntry(
        &ack_coalescing_key, *connection_metadata);
  }
}

absl::Status Gen2ConnectionStateManager::CreateConnectionState(
    std::unique_ptr<ConnectionMetadata> connection_metadata) {
  const uint32_t scid = connection_metadata->scid;
  auto connection_metadata_ptr = connection_metadata.get();
  connection_contexts_[scid] = std::make_unique<Gen2ConnectionState>(
      std::move(connection_metadata),
      std::make_unique<Gen2CongestionControlMetadata>(), falcon_->GetVersion());

  auto connection_state_ptr = connection_contexts_[scid].get();
  InitializeConnectionStateBitmaps(connection_state_ptr);
  return SetupGenSpecificConnectionState(connection_state_ptr,
                                         connection_metadata_ptr);
}

absl::Status Gen2ConnectionStateManager::SetupGenSpecificConnectionState(
    ConnectionState* connection_state,
    ConnectionMetadata* connection_metadata) {
  // In Gen2, we also need to resize the flow label and flow weight vectors to
  // the number of Gen2 flows for this connection.
  Gen2CongestionControlMetadata& congestion_control_metadata =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  uint32_t multipathing_num_flows =
      GetGen2MultipathingNumFlows(connection_metadata);
  if (multipathing_num_flows != 1 && multipathing_num_flows != 4) {
    return absl::InvalidArgumentError(
        "A Gen2 connection can only be configured with 1 flow (single path) or "
        "4 flows (multipath).");
  }
  congestion_control_metadata.gen2_flow_labels.resize(multipathing_num_flows);
  congestion_control_metadata.gen2_flow_weights.resize(multipathing_num_flows);
  congestion_control_metadata.gen2_num_acked.resize(multipathing_num_flows);
  congestion_control_metadata.gen2_last_rue_event_time.resize(
      multipathing_num_flows);
  congestion_control_metadata.gen2_outstanding_rue_event.resize(
      multipathing_num_flows);
  return absl::OkStatus();
}

void Gen2ConnectionStateManager::InitializeConnectionStateBitmaps(
    ConnectionState* connection_state) {
  auto& tx_reliability_metadata = connection_state->tx_reliability_metadata;
  tx_reliability_metadata.request_window_metadata.window =
      std::make_unique<Gen2FalconTxBitmap>(kGen2TxBitmapWidth);
  tx_reliability_metadata.data_window_metadata.window =
      std::make_unique<Gen2FalconTxBitmap>(kGen2TxBitmapWidth);

  auto& rx_reliability_metadata = connection_state->rx_reliability_metadata;
  rx_reliability_metadata.request_window_metadata.receive_window =
      std::make_unique<Gen2FalconRxBitmap>(kGen2RxBitmapWidth);
  rx_reliability_metadata.request_window_metadata.ack_window =
      std::make_unique<Gen2FalconRxBitmap>(kGen2RxBitmapWidth);
  rx_reliability_metadata.data_window_metadata.receive_window =
      std::make_unique<Gen2FalconRxBitmap>(kGen2RxBitmapWidth);
  rx_reliability_metadata.data_window_metadata.ack_window =
      std::make_unique<Gen2FalconRxBitmap>(kGen2RxBitmapWidth);
}

}  // namespace isekai
