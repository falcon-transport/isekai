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

#include "isekai/fabric/network_routing.h"

#include <limits.h>

#include <cstdint>
#include <memory>
#include <string_view>

#include "isekai/common/default_config_generator.h"
#include "isekai/common/status_util.h"
#include "isekai/fabric/packet_util.h"

namespace isekai {
namespace {
// Flag: enable_packet_discards
// Counters for packets discarded due to lack of matching flow and TTL limit.
constexpr std::string_view kStatScalarNoRouteDiscards =
    "router.routing_pipeline.no_route_discards";
constexpr std::string_view kStatScalarTtlLimitDiscards =
    "router.routing_pipeline.ttl_limit_discards";
constexpr std::string_view kStatScalarSwitchAggregatedDiscards =
    "router.routing_pipeline.total_discards";
}  // namespace

StatisticsCollectionConfig::RouterFlags
    NetworkRoutingPipeline::stats_collection_flags_;

NetworkRoutingPipeline::NetworkRoutingPipeline(
    NetworkRouterInterface* router,
    StatisticCollectionInterface* stats_collection,
    absl::string_view routing_table_filename,
    RouterConfigProfile::PortSelectionPolicy port_selection_policy)
    : router_(router), stats_collection_(stats_collection) {
  // Initializes statistic collection.
  if (stats_collection) {
    if (stats_collection_->GetConfig().has_router_flags()) {
      stats_collection_flags_ = stats_collection_->GetConfig().router_flags();
    } else {
      stats_collection_flags_ =
          DefaultConfigGenerator::DefaultRouterStatsFlags();
    }
    InitializeStatisticCollection();
  }
  // Initializes routing table.
  CHECK_OK(InitializeRoutingTable(routing_table_filename))
      << "fail to initialize routing table.";
  // Initializes output port selection scheme.
  output_port_selection_ =
      OutputPortSelectionFactory::GetOutputPortSelectionScheme(
          port_selection_policy, /*rng_seed=*/router_->intrand(ULONG_MAX));
}

void NetworkRoutingPipeline::InitializeStatisticCollection() {
  // Initializes the value of drop_packet statistic to be 0.
  if (stats_collection_flags_.enable_packet_discards()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatScalarNoRouteDiscards, no_route_discards_,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatScalarTtlLimitDiscards, ttl_limit_discards_,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatScalarSwitchAggregatedDiscards, total_discards_,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
  }
}

absl::Status NetworkRoutingPipeline::InitializeRoutingTable(
    absl::string_view routing_table_filename) {
  ASSIGN_OR_RETURN(routing_table_,
                   isekai::NetworkRoutingTable::Create(routing_table_filename));
  return absl::OkStatus();
}

absl::StatusOr<uint32_t> NetworkRoutingPipeline::GetOutputPort(
    omnetpp::cMessage* packet) {
  ASSIGN_OR_RETURN(auto table_output_options,
                   routing_table_->GetOutputOptionsAndModifyPacket(packet));
  auto static_port_found = GetPacketStaticRoutingPort(packet);
  // If a static port list are found and valid.
  if (static_port_found.ok()) {
    // Increments its static route index in the packet.
    IncrementPacketStaticRoutingIndex(packet);
    return static_port_found.value();
  } else {
    return output_port_selection_->SelectOutputPort(
        *table_output_options, packet, router_->GetRouterStage());
  }
}

void NetworkRoutingPipeline::RoutePacket(omnetpp::cMessage* packet) {
  // Checks if TTL limit is reached.
  if (!DecrementPacketTtl(packet).ok()) {
    LOG(ERROR) << "Reach TTL limit. Drop the packet.";
    if (stats_collection_flags_.enable_packet_discards()) {
      CHECK_OK(stats_collection_->UpdateStatistic(
          kStatScalarTtlLimitDiscards, ++ttl_limit_discards_,
          StatisticsCollectionConfig::SCALAR_MAX_STAT));
      CHECK_OK(stats_collection_->UpdateStatistic(
          kStatScalarSwitchAggregatedDiscards, ++total_discards_,
          StatisticsCollectionConfig::SCALAR_MAX_STAT));
    }
    delete packet;
    return;
  }

  // Gets the output port id.
  auto port_id = GetOutputPort(packet);
  if (!port_id.ok()) {
    LOG(ERROR) << "No output port is found. Drop the packet.";
    if (stats_collection_flags_.enable_packet_discards()) {
      CHECK_OK(stats_collection_->UpdateStatistic(
          kStatScalarNoRouteDiscards, ++no_route_discards_,
          StatisticsCollectionConfig::SCALAR_MAX_STAT));
      CHECK_OK(stats_collection_->UpdateStatistic(
          kStatScalarSwitchAggregatedDiscards, ++total_discards_,
          StatisticsCollectionConfig::SCALAR_MAX_STAT));
    }
    delete packet;
    return;
  }

  // Checks if the output port is connected or not. If it is not connected,
  // something is wrong in the routing pipeline algorithm or in the routing
  // table.
  CHECK(router_->gate("port$o", port_id.value())->isConnected())
      << "invalid output port: " << port_id.value();

  // Enqueues the packet in TX queue.
  router_->EnqueuePacket(packet, port_id.value());
}

}  // namespace isekai
