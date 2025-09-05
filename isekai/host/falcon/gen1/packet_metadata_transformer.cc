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

#include "isekai/host/falcon/gen1/packet_metadata_transformer.h"

#include <cstdint>
#include <memory>
#include <utility>

#include "absl/log/check.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"

namespace isekai {

Gen1PacketMetadataTransformer::Gen1PacketMetadataTransformer(
    FalconModelInterface* falcon)
    : falcon_(falcon) {}

void Gen1PacketMetadataTransformer::TransferTxPacket(
    std::unique_ptr<Packet> packet, uint32_t scid) {
  // Get a handle on the connection and transaction state along with the packet.
  ConnectionStateManager* state_manager = falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(ConnectionState* const connection_state,
                       state_manager->PerformDirectLookup(scid));
  TransformPacketMetadata(packet.get(), connection_state);
  falcon_->get_traffic_shaper()->TransferTxPacket(std::move(packet));
}

void Gen1PacketMetadataTransformer::TransformPacketMetadata(
    Packet* packet, const ConnectionState* connection_state) {
  InsertStaticPortListToPacketIfExist(packet, connection_state);
}

void Gen1PacketMetadataTransformer::InsertStaticPortListToPacketIfExist(
    Packet* packet, const ConnectionState* connection_state) {
  if (connection_state->connection_metadata->static_routing_port_lists
          .has_value()) {
    InsertStaticPortList(packet, connection_state);
    packet->metadata.static_route.current_port_index = 0;
  }
}

void Gen1PacketMetadataTransformer::InsertStaticPortList(
    Packet* packet, const ConnectionState* connection_state) {
  // In Gen1, the size of the static_routing_port_list must be 1
  // because multipathing is not supported.
  CHECK_EQ(
      connection_state->connection_metadata->static_routing_port_lists.value()
          .size(),
      1);
  packet->metadata.static_route.port_list =
      connection_state->connection_metadata->static_routing_port_lists
          .value()[0];
}

}  // namespace isekai
