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

#include "isekai/fabric/arbiter.h"

#include <memory>

#include "glog/logging.h"

namespace isekai {

std::unique_ptr<ArbiterInterface> CreateArbiter(
    RouterConfigProfile::ArbitrationScheme arbitration_scheme) {
  switch (arbitration_scheme) {
    case RouterConfigProfile::FIXED_PRIORITY:
      return std::make_unique<FixedPriorityArbiter>();
    default:
      //
      LOG(FATAL) << "unknown arbitration scheme.";
  }
}

int FixedPriorityArbiter::Arbitrate(
    const std::vector<const omnetpp::cMessage*>& packet_list) {
  for (int i = 0; i < packet_list.size(); i++) {
    if (packet_list[i]) {
      return i;
    }
  }

  return -1;
}

}  // namespace isekai
