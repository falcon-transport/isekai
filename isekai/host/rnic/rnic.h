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

#ifndef ISEKAI_HOST_RNIC_RNIC_H_
#define ISEKAI_HOST_RNIC_RNIC_H_

#include <memory>
#include <utility>

#include "absl/strings/string_view.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/host/rnic/memory_interface.h"

namespace isekai {

// The RNic class owns RDMA, FALCON and traffic shaper.
class RNic {
 public:
  RNic(std::unique_ptr<std::vector<std::unique_ptr<MemoryInterface>>> hif,
       std::unique_ptr<RdmaBaseInterface> rdma,
       std::unique_ptr<TrafficShaperInterface> traffic_shaper,
       std::unique_ptr<FalconInterface> falcon)
      : hif_(std::move(hif)),
        rdma_(std::move(rdma)),
        traffic_shaper_(std::move(traffic_shaper)),
        falcon_(std::move(falcon)) {}

  // Getters for the Isekai models.
  RdmaBaseInterface* get_rdma_model() const { return rdma_.get(); }
  TrafficShaperInterface* get_traffic_shaper() const {
    return traffic_shaper_.get();
  }
  FalconInterface* get_falcon_model() const { return falcon_.get(); }
  std::vector<std::unique_ptr<MemoryInterface>>* get_host_interface_list()
      const {
    return hif_.get();
  }

  // Registers the RDMA module and the FALCON module to Connection Manager.
  void RegisterToConnectionManager(
      absl::string_view host_id,
      ConnectionManagerInterface* connection_manager);

 private:
  // Maintaining unique_ptr to vector to have ownership of all the HIFs and
  // using unique_ptr for HostInterface to avoid copy operation.
  const std::unique_ptr<std::vector<std::unique_ptr<MemoryInterface>>> hif_;
  const std::unique_ptr<RdmaBaseInterface> rdma_;
  const std::unique_ptr<TrafficShaperInterface> traffic_shaper_;
  const std::unique_ptr<FalconInterface> falcon_;
};

};  // namespace isekai

#endif  // ISEKAI_HOST_RNIC_RNIC_H_
