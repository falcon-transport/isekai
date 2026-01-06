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

#include "isekai/host/falcon/gen2/ulp_backpressure_manager.h"

#include <cstdint>
#include <memory>

#include "absl/status/status.h"
#include "glog/logging.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen2/connection_state.h"
#include "isekai/host/falcon/gen2/ulp_backpressure_policy_alpha_carving.h"

namespace isekai {

Gen2UlpBackpressureManager::Gen2UlpBackpressureManager(
    FalconModelInterface* falcon)
    : falcon_(falcon) {
  const auto& config = falcon_->get_config()
                           ->gen2_config_options()
                           .per_connection_ulp_backpressure();
  // If per-connection backpressure is disabled, return directly.
  if (config.enable_backpressure() == false) {
    return;
  }

  switch (config.backpressure_policy_case()) {
    // Set the backpressure policy according to the config.
    case FalconConfig::PerConnectionBackpressure::kXoffPolicy: {
      const auto& xoff_config = config.xoff_policy();
      switch (xoff_config.per_connection_rdma_xoff_policy_case()) {
        case FalconConfig::PerConnectionBackpressure::XoffPolicy::
            kAlphaCarvingConfiguration:
          backpressure_policy_ =
              std::make_unique<AlphaCarvingUlpBackpressurePolicy>(
                  falcon, xoff_config.alpha_carving_configuration()
                              .tx_packet_headroom());
          break;
        default:
          LOG(FATAL) << "Unsupported xoff policy provided.";
      }
      break;
    }
    default:
      LOG(FATAL) << "Unsupported backpressure policy provided.";
  }
}

absl::Status Gen2UlpBackpressureManager::CheckAndBackpressureQP(uint32_t scid) {
  auto config = falcon_->get_config()
                    ->gen2_config_options()
                    .per_connection_ulp_backpressure();

  // If per-connection backpressure is disabled, return directly.
  if (config.enable_backpressure() == false) {
    return absl::OkStatus();
  }

  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  ASSIGN_OR_RETURN(ConnectionState * base_connection_state,
                   state_manager->PerformDirectLookup(scid));
  auto connection_state =
      dynamic_cast<Gen2ConnectionState*>(base_connection_state);

  // Currently we do not support per-connection xoff for UD.
  if (connection_state->connection_metadata->rdma_mode ==
      ConnectionMetadata::RdmaMode::kUD)
    return absl::OkStatus();

  QpId qp_id = connection_state->connection_xoff_metadata.qp_id;
  BackpressureData backpressure_data;

  switch (config.backpressure_policy_case()) {
    case FalconConfig::PerConnectionBackpressure::kXoffPolicy: {
      connection_state->connection_xoff_metadata.ulp_xoff =
          backpressure_policy_->ComputeXoffStatus(scid);
      backpressure_data.xoff =
          connection_state->connection_xoff_metadata.ulp_xoff;
      falcon_->get_rdma_model()->BackpressureQP(qp_id, BackpressureType::kXoff,
                                                backpressure_data);
      break;
    }
    default: {
      LOG(FATAL) << "Unsupported backpressure policy provided.";
    }
  }

  return absl::OkStatus();
}

}  // namespace isekai
