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

#ifndef ISEKAI_HOST_FALCON_RUE_ALGORITHM_CONNECTION_STATE_H_
#define ISEKAI_HOST_FALCON_RUE_ALGORITHM_CONNECTION_STATE_H_

#include <array>
#include <cmath>
#include <cstdint>

#include "absl/base/optimization.h"
#include "isekai/host/falcon/rue/bits.h"
#include "isekai/host/falcon/rue/fixed.h"
#include "isekai/host/falcon/rue/format_gen1.h"
#include "isekai/host/falcon/rue/format_gen2.h"

namespace isekai {
namespace rue {

inline constexpr uint8_t kUnusedBitsOfPlbState = 8;

union PlbState {
  uint32_t value;
  struct {
    uint32_t packets_congestion_acknowledged : falcon_rue::kBitsOfPlbAckCounter;
    uint32_t packets_acknowledged : falcon_rue::kBitsOfPlbAckCounter;
    uint32_t plb_reroute_attempted : falcon_rue::kBitsOfPlbAttemptCounter;
    uint32_t reserved : kUnusedBitsOfPlbState;
  };
};

//
// ConnectionState holds the per-connection state stored in DRAM required for
// processing Events in Stateful RUE.
template <typename EventT>
struct __attribute__((packed)) ABSL_CACHELINE_ALIGNED ConnectionState {};

// Specialize the struct for Gen_1 generation.
template <>
struct __attribute__((packed)) ABSL_CACHELINE_ALIGNED
    ConnectionState<falcon_rue::Event_GEN1> {
  PlbState plb_state;
  uint16_t ncwnd_fraction;
  // Padding to ensure this struct occupies a full 64B.
  uint8_t reserved[58];
};
static_assert(sizeof(ConnectionState<falcon_rue::Event_GEN1>) == 64);

//*****************************************************************************
// Gen_2 specific logic below. Used by only Isekai currently.
//*****************************************************************************

// Initial values for per-connection state fields.
const uint32_t kInitialFlowFcwnd = falcon_rue::FloatToFixed<double, uint32_t>(
    32.0, falcon_rue::kFractionalBits);
const uint32_t kInitialFlowRttState =
    std::round(25 / 0.131072);  // 25us in Falcon time units

// Holds the Event's flow-specific state that will be used for processing.
struct __attribute__((packed)) PerFlowState {
  uint32_t fcwnd_time_marker;
  uint32_t plb_state;
  uint32_t rtt_state;

  PerFlowState() {
    fcwnd_time_marker = 0;
    plb_state = 0;
    rtt_state = kInitialFlowRttState;
  }
};

// Specialize the struct for Gen_2 generation.
template <>
struct __attribute__((packed)) ABSL_CACHELINE_ALIGNED
    ConnectionState<falcon_rue::Event_Gen2> {
  // Holds both the integer and fractional parts of the fcwnd. The fcwnd of all
  // the 4 flows will be used for processing an event belonging to any of the
  // flows.
  std::array<uint32_t, 4> fcwnd = {kInitialFlowFcwnd, kInitialFlowFcwnd,
                                   kInitialFlowFcwnd, kInitialFlowFcwnd};
  // Holds the per flow state for flows 1 to 3. Flow 0's state will continue
  // being stored in the datapath.
  std::array<PerFlowState, 3> per_flow_states;

  // Padding to ensure this struct occupies a full 64B.
  uint8_t reserved[12];

  // Returns the event's flow-specific state that will be used for processing.
  // Assumes 1 <= flow_id <= 3: flow_id cannot be 0 since its state will
  // continue being stored in the datapath.
  PerFlowState& GetFlowState(uint8_t flow_id) {
    return per_flow_states[flow_id - 1];
  }
};
static_assert(sizeof(ConnectionState<falcon_rue::Event_Gen2>) == 64);

}  // namespace rue
}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_RUE_ALGORITHM_CONNECTION_STATE_H_
