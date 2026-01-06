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

#include "isekai/host/falcon/gen3/connection_state_manager.h"

#include <cstdint>
#include <memory>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "isekai/common/constants.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen2/connection_state_manager.h"
#include "isekai/host/falcon/gen2/falcon_utils.h"
#include "isekai/host/falcon/gen3/connection_state.h"
#include "isekai/host/falcon/gen3/falcon_types.h"

namespace isekai {

absl::Status Gen3ConnectionStateManager::CreateConnectionState(
    std::unique_ptr<ConnectionMetadata> connection_metadata) {
  const uint32_t scid = connection_metadata->scid;
  auto connection_metadata_ptr = connection_metadata.get();
  connection_contexts_[scid] = std::make_unique<Gen3ConnectionState>(
      std::move(connection_metadata),
      std::make_unique<Gen3CongestionControlMetadata>(), falcon_->GetVersion());
  auto connection_state_ptr = connection_contexts_[scid].get();
  InitializeConnectionStateBitmaps(connection_state_ptr);
  return SetupGenSpecificConnectionState(connection_state_ptr,
                                         connection_metadata_ptr);
}

absl::Status Gen3ConnectionStateManager::SetupGenSpecificConnectionState(
    ConnectionState* connection_state,
    ConnectionMetadata* connection_metadata) {
  Gen3CongestionControlMetadata& congestion_control_metadata =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  uint32_t multipathing_num_flows =
      GetGen2MultipathingNumFlows(connection_metadata);
  // In Gen3, we need to resize the flow fcwnd and outstanding counter vector
  // to the number of flows for this connection.
  congestion_control_metadata.gen3_flow_fcwnds.resize(multipathing_num_flows);
  congestion_control_metadata.gen3_outstanding_count.resize(
      multipathing_num_flows);
  congestion_control_metadata.gen3_flow_rtt_state.resize(
      multipathing_num_flows);
  return Gen2ConnectionStateManager::SetupGenSpecificConnectionState(
      connection_state, connection_metadata);
}

void Gen3ConnectionStateManager::InitializeConnectionStateBitmaps(
    ConnectionState* connection_state) {
  auto& tx_reliability_metadata = connection_state->tx_reliability_metadata;
  tx_reliability_metadata.request_window_metadata.window =
      std::make_unique<Gen3FalconTxBitmap>(kGen3TxBitmapWidth);
  tx_reliability_metadata.data_window_metadata.window =
      std::make_unique<Gen3FalconTxBitmap>(kGen3TxBitmapWidth);

  auto& rx_reliability_metadata = connection_state->rx_reliability_metadata;
  switch (rx_window_size_override_) {
    case 0:
      // Default to kGen2RxBitmapWidth if value unspecified (fall through).
    case kGen2RxBitmapWidth:
      rx_reliability_metadata.request_window_metadata.ack_window =
          std::make_unique<Gen2FalconRxBitmap>(kGen2RxBitmapWidth);
      rx_reliability_metadata.request_window_metadata.receive_window =
          std::make_unique<Gen2FalconRxBitmap>(kGen2RxBitmapWidth);
      rx_reliability_metadata.data_window_metadata.ack_window =
          std::make_unique<Gen2FalconRxBitmap>(kGen2RxBitmapWidth);
      rx_reliability_metadata.data_window_metadata.receive_window =
          std::make_unique<Gen2FalconRxBitmap>(kGen2RxBitmapWidth);
      break;
    case kGen3RxBitmapWidth:
      rx_reliability_metadata.request_window_metadata.ack_window =
          std::make_unique<Gen3FalconRxBitmap>(kGen3RxBitmapWidth);
      rx_reliability_metadata.request_window_metadata.receive_window =
          std::make_unique<Gen3FalconRxBitmap>(kGen3RxBitmapWidth);
      rx_reliability_metadata.data_window_metadata.ack_window =
          std::make_unique<Gen3FalconRxBitmap>(kGen3RxBitmapWidth);
      rx_reliability_metadata.data_window_metadata.receive_window =
          std::make_unique<Gen3FalconRxBitmap>(kGen3RxBitmapWidth);
      break;
    default:
      LOG(FATAL) << "Unsupported RX window size in config: "
                 << rx_window_size_override_;
  }
}

}  // namespace isekai
