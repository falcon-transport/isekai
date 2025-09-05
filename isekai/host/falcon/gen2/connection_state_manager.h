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

#ifndef ISEKAI_HOST_FALCON_GEN2_CONNECTION_STATE_MANAGER_H_
#define ISEKAI_HOST_FALCON_GEN2_CONNECTION_STATE_MANAGER_H_

#include <cstdint>

#include "absl/status/status.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/connection_state_manager.h"
#include "isekai/host/falcon/gen1/falcon_types.h"

namespace isekai {

class Gen2ConnectionStateManager : public ProtocolConnectionStateManager {
 public:
  explicit Gen2ConnectionStateManager(FalconModelInterface* falcon);

 protected:
  // Initializes the CongestionControlMetadata struct of the ConnectionState.
  absl::Status CreateConnectionState(
      std::unique_ptr<ConnectionMetadata> connection_metadata) override;
  virtual absl::Status SetupGenSpecificConnectionState(
      ConnectionState* connection_state,
      ConnectionMetadata* connection_metadata);
  void InitializeConnectionStateBitmaps(
      ConnectionState* connection_state) override;

 private:
  // Handles the ACK coalescing engine related initialization for a connection.
  void InitializeAckCoalescingEntryForConnection(
      const ConnectionMetadata* connection_metadata) override;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN2_CONNECTION_STATE_MANAGER_H_
