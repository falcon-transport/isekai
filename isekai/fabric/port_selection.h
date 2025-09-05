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

#ifndef ISEKAI_FABRIC_PORT_SELECTION_H_
#define ISEKAI_FABRIC_PORT_SELECTION_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include "isekai/common/config.pb.h"
#include "isekai/fabric/memory_management_unit.h"
#include "isekai/fabric/network_routing_table.h"
#include "omnetpp/cmessage.h"

namespace isekai {

// The abstract class for output port selection for packet.
class OutputPortSelection {
 public:
  virtual ~OutputPortSelection() {}
  // Pure virtual function for output port selection for packet. The function
  // could be override to uses different schemes, e.g. weighted cost multipath
  // selection, random selection, round robin selection.
  virtual uint32_t SelectOutputPort(
      const TableOutputOptions& output_options, omnetpp::cMessage* packet,
      int stage,
      std::function<uint64_t(uint32_t, int)> get_metadata_func =
          [](uint32_t port_id, int priority) { return 0; }) = 0;
};

// Output port selection with WCMP (weighted cost multipath selection) policy.
class WcmpOutputPortSelection : public OutputPortSelection {
 public:
  // Selects the output port from output_options for the packet. The function
  // uses weighted cost multipath selection.
  uint32_t SelectOutputPort(
      const TableOutputOptions& output_options, omnetpp::cMessage* packet,
      int stage,
      std::function<uint64_t(uint32_t, int)> get_metadata_func =
          [](uint32_t port_id, int priority) { return 0; }) override;
};

// The factory for generating object for output port selection for packet.
class OutputPortSelectionFactory {
 public:
  // Returns object for output port selection for packet according to
  // port_selection_scheme.
  // For RANDOM port selection, rng_seed is used to
  // initialize the PRNG, otherwise value is ignored.
  static std::unique_ptr<OutputPortSelection> GetOutputPortSelectionScheme(
      RouterConfigProfile::PortSelectionPolicy port_selection_scheme,
      uint32_t rng_seed = 0);
};

}  // namespace isekai

#endif  // ISEKAI_FABRIC_PORT_SELECTION_H_
