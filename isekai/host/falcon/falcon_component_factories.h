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

#ifndef ISEKAI_HOST_FALCON_FALCON_COMPONENT_FACTORIES_H_
#define ISEKAI_HOST_FALCON_FALCON_COMPONENT_FACTORIES_H_

#include <cstdint>
#include <memory>

#include "absl/log/log.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/gen1/ack_coalescing_engine.h"
#include "isekai/host/falcon/gen1/ack_nack_scheduler.h"
#include "isekai/host/falcon/gen1/admission_control_manager.h"
#include "isekai/host/falcon/gen1/buffer_reorder_engine.h"
#include "isekai/host/falcon/gen1/connection_scheduler.h"
#include "isekai/host/falcon/gen1/connection_state_manager.h"
#include "isekai/host/falcon/gen1/inter_host_rx_scheduler.h"
#include "isekai/host/falcon/gen1/packet_metadata_transformer.h"
#include "isekai/host/falcon/gen1/packet_reliability_manager.h"
#include "isekai/host/falcon/gen1/rate_update_engine.h"
#include "isekai/host/falcon/gen1/resource_manager.h"
#include "isekai/host/falcon/gen1/retransmission_scheduler.h"
#include "isekai/host/falcon/gen1/round_robin_arbiter.h"
#include "isekai/host/falcon/gen1/stats_manager.h"
#include "isekai/host/falcon/gen2/ack_coalescing_engine.h"
#include "isekai/host/falcon/gen2/connection_state_manager.h"
#include "isekai/host/falcon/gen2/falcon_component_interfaces.h"
#include "isekai/host/falcon/gen2/packet_metadata_transformer.h"
#include "isekai/host/falcon/gen2/rate_update_engine.h"
#include "isekai/host/falcon/gen2/reliability_manager.h"
#include "isekai/host/falcon/gen2/resource_manager.h"
#include "isekai/host/falcon/gen2/ulp_backpressure_manager.h"

namespace isekai {

// Factory creates the appropriate connection state manager based on
// the simulation mode.
static std::unique_ptr<ConnectionStateManager> CreateConnectionStateManager(
    FalconModelInterface* falcon) {
  auto falcon_generation = falcon->GetVersion();
  switch (falcon_generation) {
    case 1:
      return std::make_unique<ProtocolConnectionStateManager>(falcon);
    case 2:
      return std::make_unique<Gen2ConnectionStateManager>(falcon);
    default:
      LOG(FATAL) << "Invalid Falcon protocol generation.";
  }
}

// Factory creates the appropriate ACK coalescing engine based on the
// simulation mode.
inline std::unique_ptr<AckCoalescingEngineInterface> CreateAckCoalescingEngine(
    FalconModelInterface* falcon) {
  auto falcon_generation = falcon->GetVersion();
  if (falcon_generation == 1) {
    return std::make_unique<Gen1AckCoalescingEngine>(falcon);
  } else if (falcon_generation >= 2) {
    return std::make_unique<Gen2AckCoalescingEngine>(falcon);
  } else {
    LOG(FATAL) << "Invalid Falcon protocol generation.";
  }
}

// Factory creates the appropriate flow control manager based on the
// simulation mode.
static std::unique_ptr<ResourceManager> CreateResourceManager(
    FalconModelInterface* falcon) {
  auto falcon_generation = falcon->GetVersion();
  switch (falcon_generation) {
    case 1:
      return std::make_unique<ProtocolResourceManager>(falcon);
    case 2:
      return std::make_unique<Gen2ResourceManager>(falcon);
    default:
      LOG(FATAL) << "Invalid Falcon protocol generation.";
  }
}

// Factory creates the appropriate backpressure manager based on the
// simulation mode.
static std::unique_ptr<UlpBackpressureManager> CreateUlpBackpressureManager(
    FalconModelInterface* falcon) {
  auto falcon_generation = falcon->GetVersion();
  switch (falcon_generation) {
    case 1:
      LOG(FATAL) << "This version does not support this feature.";
    case 2:
      return std::make_unique<Gen2UlpBackpressureManager>(falcon);
    default:
      LOG(FATAL) << "Invalid Falcon protocol generation.";
  }
}

// Factory creates the appropriate inter-host scheduler based on the
// simulation mode.
static std::unique_ptr<InterHostRxScheduler> CreateInterHostRxScheduler(
    FalconModelInterface* falcon, const uint8_t number_of_hosts) {
  auto falcon_generation = falcon->GetVersion();
  switch (falcon_generation) {
    case 1:
    case 2:
      return std::make_unique<ProtocolInterHostRxScheduler>(falcon,
                                                            number_of_hosts);
    default:
      LOG(FATAL) << "Invalid Falcon protocol generation.";
  }
}

// Factory creates the appropriate connection scheduler based on the
// simulation mode.
static std::unique_ptr<Scheduler> CreateConnectionScheduler(
    FalconModelInterface* falcon) {
  return std::make_unique<ProtocolConnectionScheduler>(falcon);
}

// Factory creates the appropriate retransmission scheduler based on the
// simulation mode.
static std::unique_ptr<Scheduler> CreateRetransmissionScheduler(
    FalconModelInterface* falcon) {
  return std::make_unique<ProtocolRetransmissionScheduler>(falcon);
}

// Factory creates the appropriate ack/nack scheduler based on the
// simulation mode.
static std::unique_ptr<Scheduler> CreateAckNackScheduler(
    FalconModelInterface* falcon) {
  auto falcon_generation = falcon->GetVersion();
  switch (falcon_generation) {
    case 1:
    case 2:
      return std::make_unique<ProtocolAckNackScheduler>(falcon);
    default:
      LOG(FATAL) << "Invalid Falcon protocol generation.";
  }
}

// Factory creates the appropriate arbiter based on the
// simulation mode.
static std::unique_ptr<Arbiter> CreateArbiter(FalconModelInterface* falcon) {
  auto falcon_generation = falcon->GetVersion();
  switch (falcon_generation) {
    case 1:
    case 2:
      return std::make_unique<ProtocolRoundRobinArbiter>(falcon);
    default:
      LOG(FATAL) << "Invalid Falcon protocol generation.";
  }
}

// Factory creates the appropriate admission control manager based on the
// simulation mode.
static std::unique_ptr<AdmissionControlManager> CreateAdmissionControlManager(
    FalconModelInterface* falcon) {
  auto falcon_generation = falcon->GetVersion();
  switch (falcon_generation) {
    case 1:
    case 2:
      return std::make_unique<ProtocolAdmissionControlManager>(falcon);
    default:
      LOG(FATAL) << "Invalid Falcon protocol generation.";
  }
}

// Factory creates the appropriate packet reliability manager based on the
// simulation mode.
static std::unique_ptr<PacketReliabilityManager> CreatePacketReliabilityManager(
    FalconModelInterface* falcon) {
  auto falcon_generation = falcon->GetVersion();
  switch (falcon_generation) {
    case 1:
      return std::make_unique<ProtocolPacketReliabilityManager>(falcon);
    case 2:
      return std::make_unique<Gen2ReliabilityManager>(falcon);
    default:
      LOG(FATAL) << "Invalid Falcon protocol generation.";
  }
}

// Factory creates the appropriate rate update engine based on the
// simulation mode.
static std::unique_ptr<RateUpdateEngine> CreateRateUpdateEngine(
    FalconModelInterface* falcon) {
  auto falcon_generation = falcon->GetVersion();
  switch (falcon_generation) {
    case 1:
      return std::make_unique<Gen1RateUpdateEngine>(falcon);
    case 2:
      return std::make_unique<Gen2RateUpdateEngine>(falcon);
    default:
      LOG(FATAL) << "Invalid Falcon protocol generation.";
  }
}

// Factory creates the appropriate buffer reorder engine based on the
// simulation mode.
static std::unique_ptr<BufferReorderEngine> CreateBufferReorderEngine(
    FalconModelInterface* falcon) {
  auto falcon_generation = falcon->GetVersion();
  switch (falcon_generation) {
    case 1:
    case 2:
      return std::make_unique<ProtocolBufferReorderEngine>(falcon);
    default:
      LOG(FATAL) << "Invalid Falcon protocol generation.";
  }
}

// Factory creates the appropriate stats manager based on the simulation mode.
static std::unique_ptr<FalconStatsManager> CreateStatsManager(
    FalconModelInterface* falcon) {
  auto falcon_generation = falcon->GetVersion();
  switch (falcon_generation) {
    case 1:
    case 2:
      return std::make_unique<FalconStatsManager>(falcon);
    default:
      LOG(FATAL) << "Invalid Falcon protocol generation.";
  }
}

inline std::unique_ptr<PacketMetadataTransformer>
CreatePacketMetadataTransformer(FalconModelInterface* falcon) {
  auto falcon_generation = falcon->GetVersion();
  switch (falcon_generation) {
    case 1:
      return std::make_unique<Gen1PacketMetadataTransformer>(falcon);
    case 2:
      return std::make_unique<Gen2PacketMetadataTransformer>(falcon);
    default:
      LOG(FATAL) << "Invalid Falcon protocol generation.";
  }
}

}  // namespace isekai
#endif  // ISEKAI_HOST_FALCON_FALCON_COMPONENT_FACTORIES_H_
