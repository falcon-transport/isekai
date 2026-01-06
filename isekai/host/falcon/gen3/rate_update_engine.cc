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

#include "isekai/host/falcon/gen3/rate_update_engine.h"

#include <cstdint>

#include "absl/log/check.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen2/rate_update_engine.h"
#include "isekai/host/falcon/gen3/falcon_types.h"
#include "isekai/host/falcon/rue/bits.h"
#include "isekai/host/falcon/rue/fixed.h"

namespace isekai {

Gen3RateUpdateEngine::Gen3RateUpdateEngine(FalconModelInterface* falcon)
    : Gen2RateUpdateEngine(falcon) {}

void Gen3RateUpdateEngine::InitializeGenSpecificMetadata(
    CongestionControlMetadata& gen1_metadata) {
  Gen2RateUpdateEngine::InitializeGenSpecificMetadata(gen1_metadata);
  Gen3CongestionControlMetadata& metadata =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          gen1_metadata);
  // Initialize the Gen3 flow fcwnds. The function expects the size of these
  // vectors to be the number of available flows/paths for that connection.
  CHECK_EQ(metadata.gen2_flow_labels.size(), metadata.gen3_flow_fcwnds.size());
  uint32_t num_flows = metadata.gen2_flow_labels.size();
  double init_per_conn_fcwnd = falcon_rue::FixedToFloat<uint32_t, double>(
      metadata.fabric_congestion_window, falcon_rue::kFractionalBits);
  uint32_t init_per_flow_fcwnd = falcon_rue::FloatToFixed<double, uint32_t>(
      init_per_conn_fcwnd / num_flows, falcon_rue::kFractionalBits);
  for (int idx = 0; idx < num_flows; ++idx) {
    // Initialize all fcwnd to the same value (init_fcwnd / num_flows). As the
    // sender receives ACKs with congestion signals from each path, these fcwnds
    // are adjusted accordingly.
    metadata.gen3_flow_fcwnds[idx] = init_per_flow_fcwnd;
    // Initialize per-flow RTT state
    metadata.gen3_flow_rtt_state[idx] = ToFalconTimeUnits(kDefaultRttState);
  }
}

}  // namespace isekai
