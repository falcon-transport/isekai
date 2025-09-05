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
#ifndef ISEKAI_HOST_FALCON_RUE_ALGORITHM_FACTORIES_H_
#define ISEKAI_HOST_FALCON_RUE_ALGORITHM_FACTORIES_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/event_response_format_adapter.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/gen1/rate_update_engine_adapter.h"
#include "isekai/host/falcon/rue/algorithm/dram_state_manager.h"
#include "isekai/host/falcon/rue/algorithm/stateful_algorithm.h"
#include "isekai/host/falcon/rue/algorithm/swift.h"
#include "isekai/host/falcon/rue/algorithm/swift.pb.h"
#include "isekai/host/falcon/rue/format_gen1.h"
#include "isekai/host/falcon/rue/format_gen2.h"

namespace isekai {

template <typename EventT, typename ResponseT, typename StateT>
inline std::unique_ptr<RueAdapterInterface> CreateStatefulSwiftRue(
    FalconModelInterface* falcon,
    const ::isekai::rue::SwiftConfiguration& configuration,
    const StateT& default_state_value) {
  using DramStateManagerT =
      ::isekai::rue::HashMapDramStateManagerWithOnDemandInit<EventT>;
  using AlgorithmT = ::isekai::rue::Swift<EventT, ResponseT>;
  using StatefulAlgorithmT = ::isekai::rue::StatefulAlgorithm<
      /*Algorithm=*/AlgorithmT,
      /*DramStateManagerT=*/DramStateManagerT,
      /*ForBenchmarking=*/false>;
  CHECK_OK_THEN_ASSIGN(
      std::unique_ptr<StatefulAlgorithmT> swift,
      AlgorithmT::template Create<StatefulAlgorithmT>(configuration));
  auto manager =
      std::make_unique<DramStateManagerT>(sizeof(StateT), &default_state_value);
  swift->set_dram_state_manager(std::move(manager));
  auto format_adapter =
      std::make_unique<EventResponseFormatAdapter<EventT, ResponseT>>(falcon);
  return std::make_unique<RueAdapter<StatefulAlgorithmT, EventT, ResponseT>>(
      std::move(swift), std::move(format_adapter));
}

inline std::unique_ptr<RueAdapterInterface> GetSwiftRueAdapter(
    FalconModelInterface* falcon,
    const ::isekai::rue::SwiftConfiguration& configuration,
    uint32_t initial_fcwnd_fixed) {
  auto falcon_generation = falcon->GetVersion();
  if (falcon_generation == 1) {
    using EventT = falcon_rue::Event_GEN1;
    using ResponseT = falcon_rue::Response_GEN1;
    using StateT = ::isekai::rue::ConnectionState<EventT>;

    auto default_state_value = StateT{};

    return CreateStatefulSwiftRue<EventT, ResponseT, StateT>(
        falcon, configuration, default_state_value);
  } else if (falcon_generation >= 2) {
    using EventT = falcon_rue::Event_Gen2;
    using ResponseT = falcon_rue::Response_Gen2;
    using StateT = ::isekai::rue::ConnectionState<EventT>;

    auto default_state_value = StateT{};
    // In Gen2 multipathing, the initial fcwnd for a connection
    // (initial_fcwnd_fixed) is the sum of the initial flow fcwnds
    // (default_state_value.fcwnd[i]) as initialized in the Swift algorithm.
    size_t num_flows = default_state_value.fcwnd.size();
    uint32_t initial_flow_fcwnd_fixed = initial_fcwnd_fixed / num_flows;
    for (size_t flow_id = 0; flow_id < num_flows; ++flow_id) {
      default_state_value.fcwnd[flow_id] = initial_flow_fcwnd_fixed;
    }

    return CreateStatefulSwiftRue<EventT, ResponseT, StateT>(
        falcon, configuration, default_state_value);
  } else {
    LOG(FATAL) << "No support for this algorithm in other generations";
  }
}

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_RUE_ALGORITHM_FACTORIES_H_
