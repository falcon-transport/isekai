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

#include "isekai/host/falcon/gen2/falcon_model.h"

#include <cstdint>
#include <memory>

#include "absl/log/check.h"
#include "absl/strings/string_view.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon_component_factories.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen2/connection_state.h"
#include "isekai/host/falcon/gen2/falcon_types.h"
#include "isekai/host/falcon/gen2/falcon_utils.h"

namespace isekai {

Gen2FalconModel::Gen2FalconModel(const FalconConfig& configuration,
                                 Environment* env,
                                 StatisticCollectionInterface* stats_collector,
                                 ConnectionManagerInterface* connection_manager,
                                 std::string_view host_id,
                                 uint8_t number_of_hosts)
    : FalconModel(configuration, env, stats_collector, connection_manager,
                  host_id, number_of_hosts),
      ulp_backpressure_manager_(CreateUlpBackpressureManager(this)) {}

//
// as an argument to this function, or TX/RX packet context containing the
// flow_id works instead (rather than getting the flow_id from flow label in
// multiple places). Creates a Gen2OpaqueCookie and fills it with the needed
// information.
std::unique_ptr<OpaqueCookie> Gen2FalconModel::CreateCookie(
    const Packet& packet) {
  // In Gen2, we need the flow_id to be reflected inside the cookie along with a
  // ULP (N)ACK.
  uint8_t flow_id = GetFlowIdFromFlowLabel(packet.metadata.flow_label, this,
                                           packet.falcon.dest_cid);
  return std::make_unique<Gen2OpaqueCookie>(flow_id);
}

std::unique_ptr<ConnectionMetadata> Gen2FalconModel::CreateConnectionMetadata(
    uint32_t scid, uint32_t dcid, uint8_t source_bifurcation_id,
    uint8_t destination_bifurcation_id, absl::string_view dst_ip_address,
    OrderingMode ordering_mode,
    const FalconConnectionOptions& connection_options) {
  auto connection_metadata = std::make_unique<Gen2ConnectionMetadata>();
  FillConnectionMetadata(connection_metadata.get(), scid, dcid,
                         source_bifurcation_id, destination_bifurcation_id,
                         dst_ip_address, ordering_mode, connection_options);
  return connection_metadata;
}

uint32_t Gen2FalconModel::SetupNewQp(uint32_t scid, QpId qp_id, QpType qp_type,
                                     OrderingMode ordering_mode) {
  ConnectionStateManager* const state_manager =
      FalconModel::get_state_manager();
  CHECK_OK_THEN_ASSIGN(ConnectionState* const connection_state,
                       state_manager->PerformDirectLookup(scid));
  Gen2ConnectionState* const gen2_connection_state =
      dynamic_cast<Gen2ConnectionState*>(connection_state);
  gen2_connection_state->connection_xoff_metadata.qp_id = qp_id;
  CHECK(connection_state->connection_metadata->ordered_mode == ordering_mode);

  return FalconModel::SetupNewQp(scid, qp_id, qp_type, ordering_mode);
}

void Gen2FalconModel::FillConnectionMetadata(
    ConnectionMetadata* connection_metadata, uint32_t& scid, uint32_t& dcid,
    uint8_t& source_bifurcation_id, uint8_t& destination_bifurcation_id,
    absl::string_view& dst_ip_address, OrderingMode& ordering_mode,
    const FalconConnectionOptions& connection_options) {
  FalconModel::FillConnectionMetadata(
      connection_metadata, scid, dcid, source_bifurcation_id,
      destination_bifurcation_id, dst_ip_address, ordering_mode,
      connection_options);
  // Have to set degree_of_multipathing for non-Gen1 ConnectionStateTypes. In
  // this case, we expect the underlying type of FalconConnectionOptions to be
  // FalconMultipathConnectionOptions.
  auto multipath_connection_options =
      dynamic_cast<const FalconMultipathConnectionOptions&>(connection_options);
  auto gen2_connection_metadata =
      dynamic_cast<Gen2ConnectionMetadata*>(connection_metadata);
  gen2_connection_metadata->degree_of_multipathing =
      multipath_connection_options.degree_of_multipathing;
}

}  // namespace isekai
