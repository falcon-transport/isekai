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

#ifndef ISEKAI_HOST_FALCON_GEN2_PACKET_METADATA_TRANSFORMER_H_
#define ISEKAI_HOST_FALCON_GEN2_PACKET_METADATA_TRANSFORMER_H_

#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/packet_metadata_transformer.h"

namespace isekai {

class Gen2PacketMetadataTransformer : public Gen1PacketMetadataTransformer {
 public:
  explicit Gen2PacketMetadataTransformer(FalconModelInterface* falcon);

 private:
  // Handles adding any fields to the packet metadata as configured.
  void TransformPacketMetadata(
      Packet* packet, const ConnectionState* connection_state) override;

  // Inserts the static routing port list into the packet if the list exists.
  void InsertStaticPortList(Packet* packet,
                            const ConnectionState* connection_state) override;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN2_PACKET_METADATA_TRANSFORMER_H_
