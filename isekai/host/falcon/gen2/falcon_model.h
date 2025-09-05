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

#ifndef ISEKAI_HOST_FALCON_GEN2_FALCON_MODEL_H_
#define ISEKAI_HOST_FALCON_GEN2_FALCON_MODEL_H_

#include <cstdint>
#include <memory>

#include "absl/strings/string_view.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen2/falcon_component_interfaces.h"

namespace isekai {

class Gen2FalconModel : public FalconModel,
                        public Gen2FalconModelExtensionInterface {
 public:
  explicit Gen2FalconModel(const FalconConfig& configuration, Environment* env,
                           StatisticCollectionInterface* stats_collector,
                           ConnectionManagerInterface* connection_manager,
                           std::string_view host_id, uint8_t number_of_hosts);
  // This is called by the RDMA model when a new QP is set up.
  uint32_t SetupNewQp(uint32_t scid, QpId qp_id, QpType qp_type,
                      OrderingMode ordering_mode) override;
  UlpBackpressureManager* get_ulp_backpressure_manager() const override {
    return ulp_backpressure_manager_.get();
  }

 private:
  // Creates the cookie that flows from Falcon to the ULP and then back to
  // Falcon in a ULP ACK.
  std::unique_ptr<OpaqueCookie> CreateCookie(const Packet& packet) override;
  std::unique_ptr<ConnectionMetadata> CreateConnectionMetadata(
      uint32_t scid, uint32_t dcid, uint8_t source_bifurcation_id,
      uint8_t destination_bifurcation_id, absl::string_view dst_ip_address,
      OrderingMode ordering_mode,
      const FalconConnectionOptions& options) override;
  void FillConnectionMetadata(
      ConnectionMetadata* metadata, uint32_t& scid, uint32_t& dcid,
      uint8_t& source_bifurcation_id, uint8_t& destination_bifurcation_id,
      absl::string_view& dst_ip_address, OrderingMode& ordering_mode,
      const FalconConnectionOptions& connection_options);

  const std::unique_ptr<UlpBackpressureManager> ulp_backpressure_manager_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN2_FALCON_MODEL_H_
