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

#ifndef ISEKAI_HOST_FALCON_GEN2_FALCON_UTILS_H_
#define ISEKAI_HOST_FALCON_GEN2_FALCON_UTILS_H_

#include <cstdint>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"
#include "isekai/host/falcon/gen2/falcon_types.h"

namespace isekai {

// Gets the flow's ID from the flow label and the configured number of flows per
// connection. The Flow ID is represented by the LSBs in the flow label, where
// the number of LSBs depend on the configured num_flows_per_connection.
inline uint8_t GetFlowIdFromFlowLabel(uint32_t flow_label,
                                      uint8_t num_flows_per_connection) {
  return static_cast<uint8_t>(flow_label % num_flows_per_connection);
}

// The Gen2MultipathingNumFlows value is the number of flows (paths) per
// connection for Gen2 multipathing.  A value of 1  means that only one
// path/flow is used at a time by a connection, but still goes through all
// Gen2-multipathing-related code changes.
inline uint32_t GetGen2MultipathingNumFlows(
    const ConnectionMetadata* connection_metadata) {
  auto gen2_metadata_ptr =
      dynamic_cast<const Gen2ConnectionMetadata*>(connection_metadata);
  CHECK_GT(gen2_metadata_ptr->degree_of_multipathing, 0);
  return gen2_metadata_ptr->degree_of_multipathing;
}

// Gets the flow's ID from the flow label and the FalconInterface as well as the
// scid. We use scid to get ConnectionState from the ConnectionStateManager in
// FalconInterface. From ConnectionState, we fetch the configured number of
// flows for that connection.
inline uint8_t GetFlowIdFromFlowLabel(uint32_t flow_label,
                                      const FalconModelInterface* falcon,
                                      uint32_t scid) {
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       falcon->get_state_manager()->PerformDirectLookup(scid));
  uint32_t multipathing_num_flows =
      GetGen2MultipathingNumFlows(connection_state->connection_metadata.get());
  return GetFlowIdFromFlowLabel(flow_label, multipathing_num_flows);
}

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN2_FALCON_UTILS_H_
