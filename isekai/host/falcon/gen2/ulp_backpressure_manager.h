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

#ifndef ISEKAI_HOST_FALCON_GEN2_ULP_BACKPRESSURE_MANAGER_H_
#define ISEKAI_HOST_FALCON_GEN2_ULP_BACKPRESSURE_MANAGER_H_

#include <cstdint>
#include <memory>

#include "absl/status/status.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/gen2/falcon_component_interfaces.h"

namespace isekai {

// Manage backpressure between Falcon and RDMA. It backpressures upstream QPs
// based on the configured policy.
class Gen2UlpBackpressureManager : public UlpBackpressureManager {
 public:
  explicit Gen2UlpBackpressureManager(FalconModelInterface* falcon);
  // Backpressure a QP.
  absl::Status CheckAndBackpressureQP(uint32_t scid) override;

 private:
  FalconModelInterface* const falcon_;
  std::unique_ptr<PerConnectionUlpBackpressurePolicy> backpressure_policy_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN2_ULP_BACKPRESSURE_MANAGER_H_
