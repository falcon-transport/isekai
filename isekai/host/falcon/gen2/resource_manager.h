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

#ifndef ISEKAI_HOST_FALCON_GEN2_RESOURCE_MANAGER_H_
#define ISEKAI_HOST_FALCON_GEN2_RESOURCE_MANAGER_H_

#include <cstdint>

#include "absl/status/status.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/resource_manager.h"

namespace isekai {

class Gen2ResourceManager : public ProtocolResourceManager {
 public:
  explicit Gen2ResourceManager(FalconModelInterface* falcon);
  absl::Status VerifyResourceAvailabilityOrReserveResources(
      uint32_t scid, const Packet* packet, PacketDirection direction,
      bool reserve_resources) override;

 private:
  void ReturnRdmaManagedFalconResourceCredits(
      ConnectionState* connection_state, const TransactionKey& transaction_key,
      FalconCredit rdma_managed_resource_credits) override;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN2_RESOURCE_MANAGER_H_
