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

#ifndef ISEKAI_FABRIC_NETWORK_ROUTING_H_
#define ISEKAI_FABRIC_NETWORK_ROUTING_H_

#include <cstdint>
#include <memory>

#include "gtest/gtest_prod.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/fabric/model_interfaces.h"
#include "isekai/fabric/network_routing_table.h"
#include "isekai/fabric/port_selection.h"

namespace isekai {

class NetworkRoutingPipeline : public RoutingPipelineInterface {
 public:
  NetworkRoutingPipeline(
      NetworkRouterInterface* router,
      StatisticCollectionInterface* stats_collection,
      absl::string_view routing_table_filename,
      RouterConfigProfile::PortSelectionPolicy port_selection_policy);
  void RoutePacket(omnetpp::cMessage* packet) override;

 private:
  // Initializes the routing table.
  absl::Status InitializeRoutingTable(absl::string_view routing_table_filename);
  // Initializes the statistic collection model in the NetworkRouter.
  void InitializeStatisticCollection();
  // Determines the output port for the rx packet.
  absl::StatusOr<uint32_t> GetOutputPort(omnetpp::cMessage* packet);

  // The routing pipeline holds a pointer to the router so that it can call
  // functions from other router components.
  NetworkRouterInterface* const router_;
  // The NetworkRouting owns the pointer to the corresponding instance of
  // OutputPortSelection class.
  std::unique_ptr<OutputPortSelection> output_port_selection_;
  // The NetworkRouting owns the pointer to the corresponding routing table.
  std::unique_ptr<NetworkRoutingTable> routing_table_;
  StatisticCollectionInterface* const stats_collection_;
  static StatisticsCollectionConfig::RouterFlags stats_collection_flags_;
  // The number of TX, RX and dropped packets.
  // The number of dropped packets due to lack of flows.
  uint64_t no_route_discards_ = 0;
  // Tracks the number of dropped packets due to TTL limit.
  uint64_t ttl_limit_discards_ = 0;
  // Tracks the total number of dropped packets.
  uint64_t total_discards_ = 0;

  friend class NetworkRoutingPipelineTestPeer;
};

}  // namespace isekai

#endif  // ISEKAI_FABRIC_NETWORK_ROUTING_H_
