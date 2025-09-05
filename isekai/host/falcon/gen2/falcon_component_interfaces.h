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

#ifndef ISEKAI_HOST_FALCON_GEN2_FALCON_COMPONENT_INTERFACES_H_
#define ISEKAI_HOST_FALCON_GEN2_FALCON_COMPONENT_INTERFACES_H_

#include <cstdint>

#include "absl/status/status.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/host/rnic/memory_interface.h"

namespace isekai {

// Manages per-connection backpressure between Falcon and ULP. It backpressures
// upstream QPs based on the configured policy.
class UlpBackpressureManager {
 public:
  virtual ~UlpBackpressureManager() {}
  // Checks the connection state and backpressure the corresponding QP, if
  // required, as defined by the per connection RDMA backpressure policy.
  virtual absl::Status CheckAndBackpressureQP(uint32_t scid) = 0;
};

// It determines when to backpressure upstream QPs.
class PerConnectionUlpBackpressurePolicy {
 public:
  virtual ~PerConnectionUlpBackpressurePolicy() {}
  // Computes Xoff (request and response) status for the given connection.
  virtual UlpXoff ComputeXoffStatus(uint32_t scid) = 0;
};

class Gen2FalconModelExtensionInterface {
 public:
  virtual ~Gen2FalconModelExtensionInterface() = default;
  virtual UlpBackpressureManager* get_ulp_backpressure_manager() const = 0;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN2_FALCON_COMPONENT_INTERFACES_H_
