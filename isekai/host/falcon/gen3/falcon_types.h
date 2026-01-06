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

#ifndef ISEKAI_HOST_FALCON_GEN3_FALCON_TYPES_H_
#define ISEKAI_HOST_FALCON_GEN3_FALCON_TYPES_H_

#include <cstdint>
#include <vector>

#include "isekai/host/falcon/gen2/falcon_types.h"

namespace isekai {

struct Gen3CongestionControlMetadata : Gen2CongestionControlMetadata {
  Gen3CongestionControlMetadata() {}
  // These fields are Gen3-specific.
  // Per-flow fabric congestion window.
  std::vector<uint32_t> gen3_flow_fcwnds;
  // Per-flow number of outstanding packet counter.
  std::vector<uint32_t> gen3_outstanding_count;
  //
  // Per-flow smoothed RTT. This can be removed once we move RACK/TLP logic
  // to RUE.
  std::vector<uint32_t> gen3_flow_rtt_state;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN3_FALCON_TYPES_H_
