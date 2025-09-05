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

#ifndef ISEKAI_FABRIC_ARBITER_H_
#define ISEKAI_FABRIC_ARBITER_H_

#include <vector>

#include "absl/strings/string_view.h"
#include "isekai/common/config.pb.h"
#include "isekai/fabric/model_interfaces.h"
#include "omnetpp/cmessage.h"

namespace isekai {

// Create an arbiter based on the given scheme, e.g., fixed priority arbitration
// or weighted round robin arbitration.
std::unique_ptr<ArbiterInterface> CreateArbiter(
    RouterConfigProfile::ArbitrationScheme arbitration_scheme);

// The class for priority arbiter.
class FixedPriorityArbiter : public ArbiterInterface {
 public:
  // Arbitrate TX packet based on packet priority.
  int Arbitrate(
      const std::vector<const omnetpp::cMessage*>& packet_list) override;
};

}  // namespace isekai

#endif  // ISEKAI_FABRIC_ARBITER_H_
