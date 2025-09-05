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

#include "isekai/host/falcon/gen2/packet_metadata_transformer.h"

#include <cstdint>

#include "absl/log/check.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/packet_metadata_transformer.h"
#include "isekai/host/falcon/gen2/falcon_utils.h"

namespace isekai {

Gen2PacketMetadataTransformer::Gen2PacketMetadataTransformer(
    FalconModelInterface* falcon)
    : Gen1PacketMetadataTransformer(falcon) {}

void Gen2PacketMetadataTransformer::TransformPacketMetadata(
    Packet* packet, const ConnectionState* connection_state) {
  // Apply all transforms from Gen1.
  Gen1PacketMetadataTransformer::TransformPacketMetadata(packet,
                                                         connection_state);
}

void Gen2PacketMetadataTransformer::InsertStaticPortList(
    Packet* packet, const ConnectionState* connection_state) {
  // The degree of multipathing should be equal to the size of the vector of
  // static routing port lists.
  CHECK(GetGen2MultipathingNumFlows(
            connection_state->connection_metadata.get()) ==
        connection_state->connection_metadata->static_routing_port_lists.value()
            .size());
  // In Gen2, the index of the static route list corresponds to the flow ID,
  // where the flow ID is the lower N bits of the flow label, with N being equal
  // to log2(degree_multipathing).
  auto flow_id =
      GetFlowIdFromFlowLabel(packet->metadata.flow_label, falcon_,
                             connection_state->connection_metadata->scid);
  packet->metadata.static_route.port_list =
      connection_state->connection_metadata->static_routing_port_lists
          .value()[flow_id];
}

}  // namespace isekai
