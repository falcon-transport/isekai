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

#include "isekai/host/falcon/gen2/resource_manager.h"

#include <cstdint>

#include "absl/log/check.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/resource_manager.h"
#include "isekai/host/falcon/gen2/falcon_component_interfaces.h"

namespace isekai {

Gen2ResourceManager::Gen2ResourceManager(FalconModelInterface* falcon)
    : ProtocolResourceManager(falcon) {}

absl::Status Gen2ResourceManager::VerifyResourceAvailabilityOrReserveResources(
    uint32_t scid, const Packet* packet, PacketDirection direction,
    bool reserve_resources) {
  auto status =
      ProtocolResourceManager::VerifyResourceAvailabilityOrReserveResources(
          scid, packet, direction, reserve_resources);
  // In case we reserve resources for packets sent by ULP (determined by
  // packet_type being invalid as it is set later on), check if per-connection
  // backpressure needs to be applied.
  if ((falcon_->GetVersion() >= 2) && (reserve_resources == true) &&
      (status.ok()) && (packet->packet_type == falcon::PacketType::kInvalid)) {
    // Call the per-connection backpressure manager to check whether
    // backpressure this connection.
    CHECK(direction == PacketDirection::kOutgoing);
    // Get a handle on the connection state.
    ConnectionStateManager* const state_manager = falcon_->get_state_manager();
    ASSIGN_OR_RETURN(ConnectionState* const connection_state,
                     state_manager->PerformDirectLookup(scid));
    UlpBackpressureManager* const ulp_backpressure_manager =
        dynamic_cast<Gen2FalconModelExtensionInterface*>(falcon_)
            ->get_ulp_backpressure_manager();
    CHECK_OK(ulp_backpressure_manager->CheckAndBackpressureQP(
        connection_state->connection_metadata->scid));
  }
  return status;
}

void Gen2ResourceManager::ReturnRdmaManagedFalconResourceCredits(
    ConnectionState* const connection_state,
    const TransactionKey& transaction_key,
    FalconCredit rdma_managed_resource_credits) {
  if (falcon_->GetVersion() >= 2) {
    // Call the per-connection backpressure manager to check whether
    // backpressure this connection.
    UlpBackpressureManager* const ulp_backpressure_manager =
        dynamic_cast<Gen2FalconModelExtensionInterface*>(falcon_)
            ->get_ulp_backpressure_manager();
    CHECK_OK(ulp_backpressure_manager->CheckAndBackpressureQP(
        connection_state->connection_metadata->scid));
  }
  ProtocolResourceManager::ReturnRdmaManagedFalconResourceCredits(
      connection_state, transaction_key, rdma_managed_resource_credits);
}

}  // namespace isekai
