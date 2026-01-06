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

#ifndef ISEKAI_HOST_FALCON_GEN3_CONNECTION_STATE_MANAGER_H_
#define ISEKAI_HOST_FALCON_GEN3_CONNECTION_STATE_MANAGER_H_

#include <memory>

#include "absl/status/status.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen2/connection_state_manager.h"

namespace isekai {

class Gen3ConnectionStateManager : public Gen2ConnectionStateManager {
 public:
  explicit Gen3ConnectionStateManager(FalconModelInterface* falcon)
      : Gen2ConnectionStateManager(falcon),
        rx_window_size_override_(
            falcon->get_config()->gen3_config_options().rx_window_size()) {}

 protected:
  absl::Status SetupGenSpecificConnectionState(
      ConnectionState* connection_state,
      ConnectionMetadata* connection_metadata) override;

 private:
  // Initializes the CongestionControlMetadata struct of the ConnectionState.
  absl::Status CreateConnectionState(
      std::unique_ptr<ConnectionMetadata> connection_metadata) override;
  void InitializeConnectionStateBitmaps(
      ConnectionState* connection_state) override;

 private:
  // RX window size optionally specified in config; zero if unspecified.
  int rx_window_size_override_ = 0;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN3_CONNECTION_STATE_MANAGER_H_
