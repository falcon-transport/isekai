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

#ifndef ISEKAI_TESTING_INTEGRATION_OMNEST_FALCON_HOST_H_
#define ISEKAI_TESTING_INTEGRATION_OMNEST_FALCON_HOST_H_

#include "isekai/host/rnic/omnest_host.h"

// The host class that use a fake rdma model to send packets to the network
// directly.
class FalconHost : public OmnestHost {
 public:
  absl::Status ValidateSimulationConfigForTesting(
      const isekai::SimulationConfig& config) {
    return ValidateSimulationConfig(config);
  }

 protected:
  void initialize() override;
  void initialize(int stage) override;
  // Two stages to initialize FALCON host. We need to get RDMA and FALCON
  // registered to connection manager in the first stage, so that in the second
  // stage the connection manager can create QP at both ends. The first stage is
  // to initialize the components, such as rdma, falcon and packet builder, for
  // FALCON host. The second stage is to initialize traffic generator and to
  // generate RDMA traffics.
  int numInitStages() const override { return 2; }

 private:
  // Validates simulation configurations.
  static absl::Status ValidateSimulationConfig(
      const isekai::SimulationConfig& config);
};

#endif  // ISEKAI_TESTING_INTEGRATION_OMNEST_FALCON_HOST_H_
