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

#include "isekai/fabric/port_selection.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "glog/logging.h"
#include "isekai/fabric/constants.h"
#include "isekai/fabric/network_routing_table.h"
#include "isekai/fabric/packet_util.h"
#include "omnetpp/cmessage.h"

namespace isekai {
namespace {

constexpr int kS1 = 1;
constexpr int kS2 = 2;
constexpr int kS3 = 3;

uint32_t RotateLeftBits(uint32_t value, unsigned int bit_count) {
  const unsigned int mask = CHAR_BIT * sizeof(value) - 1;
  bit_count &= mask;
  return (value << bit_count) | (value >> (-bit_count & mask));
}

uint32_t HashValueRotation(uint32_t hash_value, int stage, bool uplink) {
  switch (stage) {
    case kS1: {
      return hash_value;
    }
    case kS2: {
      if (uplink) {
        return RotateLeftBits(hash_value, 4);
      } else {
        return RotateLeftBits(hash_value, 16);
      }
    }
    case kS3: {
      if (uplink) {
        return RotateLeftBits(hash_value, 8);
      } else {
        return RotateLeftBits(hash_value, 12);
      }
    }
    default:
      LOG(FATAL) << "Unknown stage: " << stage;
  }
}

// Simple example of selecting output port via WCMP:
//   mod_hash_value = 4.
//   output_options = {{/* weight */ 2, /* port_index */ 1}, {1, 2}, {4, 3},
//   {3, 4}}.
//   Then select port_index 3.
// Assumptions:
//   1. weight > 0.
//   2. The sum of weights can be hold in uint32.
uint32_t SelectWcmpOutputPort(uint32_t hash_value,
                              const TableOutputOptions& output_options) {
  size_t number_of_output_choices = output_options.size();
  uint32_t weight_sum = 0;
  for (size_t i = 0; i < number_of_output_choices; i++) {
    weight_sum += output_options[i].weight;
  }
  uint32_t mod_hash_value = hash_value % weight_sum;

  for (size_t i = 0; i < number_of_output_choices; i++) {
    if (mod_hash_value < output_options[i].weight) {
      return output_options[i].port_index;
    }
    mod_hash_value -= output_options[i].weight;
  }

  LOG(FATAL) << "No output port found.";
  return 0;
}

}  // namespace

std::unique_ptr<OutputPortSelection>
OutputPortSelectionFactory::GetOutputPortSelectionScheme(
    RouterConfigProfile::PortSelectionPolicy port_selection_scheme,
    uint32_t rng_seed) {
  switch (port_selection_scheme) {
    case RouterConfigProfile::WCMP:
      return std::make_unique<WcmpOutputPortSelection>();
    default:
      LOG(FATAL) << "Unimplemented port selection scheme.";
  }
}

uint32_t WcmpOutputPortSelection::SelectOutputPort(
    const TableOutputOptions& output_options, omnetpp::cMessage* packet,
    int stage, std::function<uint64_t(uint32_t, int)> get_metadata_func) {
  uint32_t hash_value = GenerateHashValueForPacket(packet);
  if (GetPacketSourceMac(packet) == kFlowSrcMacVrfUp) {
    hash_value = HashValueRotation(hash_value, stage, true);
  } else {
    hash_value = HashValueRotation(hash_value, stage, false);
  }

  return SelectWcmpOutputPort(hash_value, output_options);
}

}  // namespace isekai
