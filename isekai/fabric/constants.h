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

#ifndef ISEKAI_FABRIC_CONSTANTS_H_
#define ISEKAI_FABRIC_CONSTANTS_H_

#include <cstdint>

#include "isekai/common/net_address.h"

namespace isekai {

// The number of bits for one PFC pause unit.
constexpr uint32_t kPauseUnitBits = 512;

// The number of CoS supported by PFC frame.
constexpr uint32_t kClassesOfService = 8;

// The size of a cell in bits.
constexpr double kCellSize = 256 * 8;

constexpr uint32_t kDefaultNetworkMtuSize = 4096;  // 4k bytes

// Source MAC for flow with VRF UP.
const MacAddress kFlowSrcMacVrfUp(0x02, 0x00, 0x00, 0x00, 0x00, 0x01);
// Source MAC for flow with VRF DOWN.
const MacAddress kFlowSrcMacVrfDown(0x02, 0x00, 0x00, 0x00, 0x00, 0x02);

// See ECN spec:
// https://en.wikipedia.org/wiki/Explicit_Congestion_Notification#Operation_of_ECN_with_IP
enum class EcnCode : uint16_t {
  kNonEct = 0,
  kEcnCapableTransport0 = 1,
  kEcnCapableTransport1 = 2,
  kCongestionEncountered = 3,
};

struct IngressMemoryOccupancy {
  bool drop;
  uint64_t minimal_guarantee_occupancy;
  uint64_t shared_pool_occupancy;
  uint64_t headroom_occupancy;

  IngressMemoryOccupancy(bool drop, uint64_t minimal_guarantee_occupancy,
                         uint64_t shared_pool_occupancy,
                         uint64_t headroom_occupancy)
      : drop(drop),
        minimal_guarantee_occupancy(minimal_guarantee_occupancy),
        shared_pool_occupancy(shared_pool_occupancy),
        headroom_occupancy(headroom_occupancy) {}
  IngressMemoryOccupancy() {}
};

}  // namespace isekai

#endif  // ISEKAI_FABRIC_CONSTANTS_H_
