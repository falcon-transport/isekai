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

#include "isekai/fabric/network_router.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "absl/strings/string_view.h"
#include "glog/logging.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/file_util.h"
#include "isekai/common/status_util.h"
#include "isekai/fabric/memory_management_unit.h"
#include "isekai/fabric/model_interfaces.h"
#include "isekai/fabric/network_port.h"
#include "isekai/fabric/network_port_stats_m.h"
#include "isekai/fabric/network_routing.h"
#include "isekai/fabric/output_schedule_message_m.h"
#include "isekai/fabric/packet_util.h"
#include "isekai/fabric/pfc_message_m.h"
#include "isekai/host/rnic/omnest_stats_collection.h"
#include "omnetpp/cdataratechannel.h"
#include "omnetpp/checkandcast.h"
#include "omnetpp/cmessage.h"
#include "omnetpp/cpar.h"
#include "omnetpp/csimulation.h"
#include "omnetpp/regmacros.h"
#include "omnetpp/simtime.h"
#include "omnetpp/simtime_t.h"

Define_Module(NetworkRouter);

namespace {

isekai::RouterConfigProfile GetRouterConfigProfile(
    absl::string_view router_name, const isekai::NetworkConfig& network) {
  for (auto const& router_config : network.routers()) {
    if (router_name == router_config.router_name()) {
      switch (router_config.config_case()) {
        case isekai::RouterConfig::ConfigCase::kRouterConfig: {
          return router_config.router_config();
        }
        case isekai::RouterConfig::ConfigCase::kRouterConfigProfileName: {
          // Gets the host config according to the profile name
          for (const auto& profile : network.router_configs()) {
            if (profile.profile_name() ==
                router_config.router_config_profile_name()) {
              return profile;
            }
          }
          LOG(FATAL) << "no profile is found: "
                     << router_config.router_config_profile_name();
        }
        case isekai::HostConfig::ConfigCase::CONFIG_NOT_SET: {
          LOG(INFO) << "router: " << router_name << " uses the default config.";
          return {};
        }
      }
    }
  }
  LOG(FATAL) << "router does not exist: " << router_name;
}

}  // namespace

isekai::StatisticsCollectionConfig::RouterFlags
    NetworkRouter::stats_collection_flags_;

NetworkRouter::~NetworkRouter() {
  for (int i = 0; i < output_schedule_messages_.size(); i++) {
    cancelAndDelete(output_schedule_messages_[i]);
  }
  if (enable_port_stats_collection_ && port_stats_collection_interval_us_ > 0) {
    for (int i = 0; i < clct_port_stats_messages_.size(); i++) {
      cancelAndDelete(clct_port_stats_messages_[i]);
    }
  }
}

void NetworkRouter::InitializeMemoryManagementUnit() {
  memory_management_unit_ = std::make_unique<isekai::MemoryManagementUnit>(
      this, par("num_port").intValue(), tx_queue_num_, mmu_config_, mtu_size_,
      stats_collection_.get());
}

void NetworkRouter::InitializeRoutingPipeline(
    absl::string_view routing_table_path) {
  router_delay_ =
      omnetpp::simtime_t(routing_pipeline_delay_ns_, omnetpp::SIMTIME_NS);
  routing_pipeline_ = std::make_unique<isekai::NetworkRoutingPipeline>(
      /* router = */ this, /* stats_collection = */ stats_collection_.get(),
      /* routing_table_filename = */ routing_table_path,
      /* port_selection_policy = */ port_selection_policy_);
}

void NetworkRouter::InitializePorts() {
  int number_of_ports = par("num_port").intValue();
  for (int i = 0; i < number_of_ports; i++) {
    auto port = std::make_unique<isekai::NetworkPort>(
        /* router = */ this, /* port_id = */ i,
        /* number_of_queues = */ tx_queue_num_,
        /* arbitration_scheme = */
        arbitration_scheme_,
        /* stats_collection = */ stats_collection_.get(),
        // clang-format off
        enable_per_flow_round_robin_);
    // clang-format on
    ports_.push_back(std::move(port));

    auto port_output_schedule_message = new OutputScheduleMessage();
    port_output_schedule_message->setPortId(i);
    port_output_schedule_message->setKind(
        static_cast<int16_t>(isekai::RouterEventType::kOutputScheduling));
    output_schedule_messages_.push_back(port_output_schedule_message);
    if (enable_port_stats_collection_ &&
        port_stats_collection_interval_us_ > 0) {
      // Set up periodic port stats collection.
      auto port_stats_message = new CollectPortStatisticsMessage();
      port_stats_message->setPortId(i);
      port_stats_message->setKind(static_cast<int16_t>(
          isekai::RouterEventType::kCollectPortStatistics));
      clct_port_stats_messages_.push_back(port_stats_message);
      scheduleAt(omnetpp::simTime() +
                     omnetpp::SimTime(port_stats_collection_interval_us_,
                                      omnetpp::SIMTIME_US),
                 port_stats_message);
    }
  }
}

void NetworkRouter::InitializeStatsCollection(
    isekai::SimulationConfig simulation_config) {
  stats_collection_ = std::make_unique<isekai::OmnestStatisticCollection>(
      isekai::OmnestStatisticCollection::kRouterStats, this,
      simulation_config.stats_collection());
}

void NetworkRouter::initialize() {
  router_name_ = par("router_name").stringValue();
  absl::string_view routing_table = par("routing_table").stringValue();
  router_stage_ = par("stage").intValue();

  isekai::SimulationConfig simulation;
  std::string simulation_config_file =
      getSystemModule()->par("simulation_config").stringValue();
  CHECK(isekai::ReadTextProtoFromFile(simulation_config_file, &simulation).ok())
      << "fails to load simulation config proto text.";
  auto router_config =
      GetRouterConfigProfile(router_name_, simulation.network());
  // Required Configurations.
  if (!router_config.has_tx_queue_num()) {
    LOG(FATAL) << "Router config profile must specify tx_queue_num";
  }
  tx_queue_num_ = router_config.tx_queue_num();

  if (!router_config.has_mmu_config()) {
    LOG(FATAL) << "Router config profile must specify mmu_config";
  }
  mmu_config_ = router_config.mmu_config();

  if (simulation.has_stats_collection() &&
      simulation.stats_collection().has_router_flags()) {
    stats_collection_flags_ = simulation.stats_collection().router_flags();
    enable_port_stats_collection_ =
        stats_collection_flags_.enable_port_stats_collection();
    if (stats_collection_flags_.has_port_stats_collection_interval_us()) {
      port_stats_collection_interval_us_ =
          stats_collection_flags_.port_stats_collection_interval_us();
      CHECK(port_stats_collection_interval_us_ != 0)
          << "Port stats collection interval cannot be non-zero.";
    }
  } else {
    stats_collection_flags_ =
        isekai::DefaultConfigGenerator::DefaultRouterStatsFlags();
  }
  if (router_config.has_routing_pipeline_delay_ns()) {
    routing_pipeline_delay_ns_ = router_config.routing_pipeline_delay_ns();
  }
  if (router_config.has_port_selection_policy()) {
    port_selection_policy_ = router_config.port_selection_policy();
  }
  if (router_config.has_arbitration_scheme()) {
    arbitration_scheme_ = router_config.arbitration_scheme();
  }
  if (router_config.has_per_flow_round_robin()) {
    enable_per_flow_round_robin_ = router_config.per_flow_round_robin();
  }
  InitializeStatsCollection(simulation);
  InitializeMemoryManagementUnit();
  InitializeRoutingPipeline(routing_table);
  InitializePorts();
}

void NetworkRouter::RoutePacket(omnetpp::cMessage* packet) {
  // Schedules an event to ask routing pipeline to handle the packet after a
  // certain delay.
  packet->setKind(static_cast<int16_t>(isekai::RouterEventType::kRouterDelay));
  scheduleAt(omnetpp::simTime() + router_delay_, packet);
}

void NetworkRouter::EnqueuePacket(omnetpp::cMessage* packet, int port_id) {
  // Enqueues the packet. The port will enqueue the packet and trigger an output
  // event if its state is idle.
  ports_[port_id]->EnqueuePacket(packet);
}

void NetworkRouter::ScheduleOutput(int port_id,
                                   const omnetpp::simtime_t& delay) {
  auto output_schedule_message = output_schedule_messages_[port_id];
  CHECK(!output_schedule_message->isScheduled())
      << "The output_schedule message is already scheduled.";
  scheduleAt(omnetpp::simTime() + delay, output_schedule_message);
}

void NetworkRouter::SendPacket(
    omnetpp::cMessage* packet, int port_id,
    omnetpp::cDatarateChannel* transmission_channel) {
  // Sends out the packet to the network via port[port_id_]. Restores the kind
  // of the message to network packet so that it can be handled properly by the
  // next hop.
  packet->setKind(
      static_cast<int16_t>(isekai::RouterEventType::kReceivePacket));
  send(isekai::EncapsulateEthernetSignal(packet), "port$o", port_id);
}

void NetworkRouter::SetPfcPaused(
    PfcMessage* pfc_pause_msg,
    const omnetpp::simtime_t& start_pfc_pause_delay) {
  // Starts to set PFC pause after the predefined rx_pfc_delay_ time.
  scheduleAt(omnetpp::simTime() + start_pfc_pause_delay, pfc_pause_msg);
}

void NetworkRouter::handleMessage(omnetpp::cMessage* msg) {
  auto event_type = static_cast<isekai::RouterEventType>(msg->getKind());
  switch (event_type) {
    case isekai::RouterEventType::kReceivePacket: {
      // The msg is a network packet whose kind value is 0 by default.
      int port_index = GetArrivalPortId(msg);
      ports_[port_index]->ReceivePacket(msg);
      break;
    }
    case isekai::RouterEventType::kRouterDelay: {
      routing_pipeline_->RoutePacket(msg);
      break;
    }
    case isekai::RouterEventType::kOutputScheduling: {
      // Extracts the port index information carried by the self-message with
      // kind = 2.
      int port_indx =
          omnetpp::check_and_cast<OutputScheduleMessage*>(msg)->getPortId();
      // Start to transmit the next Tx packet if there is any.
      ports_[port_indx]->ScheduleOutput();
      break;
    }
    case isekai::RouterEventType::kSetPfcPaused: {
      auto pfc_set_pause_msg = omnetpp::check_and_cast<PfcMessage*>(msg);
      memory_management_unit_->SetPfcPaused(
          pfc_set_pause_msg->getPortId(), pfc_set_pause_msg->getPriority(),
          omnetpp::simTime() +
              omnetpp::simtime_t(pfc_set_pause_msg->getPauseDuration(),
                                 omnetpp::SIMTIME_NS));
      break;
    }
    case isekai::RouterEventType::kPfcRefreshment: {
      auto pfc_refreshment_msg = omnetpp::check_and_cast<PfcMessage*>(msg);
      ports_[pfc_refreshment_msg->getPortId()]->ResumeOutput();
      break;
    }
    case isekai::RouterEventType::kPfcPauseSend: {
      auto pfc_send_pause_msg = omnetpp::check_and_cast<PfcMessage*>(msg);
      memory_management_unit_->SendPfc(pfc_send_pause_msg->getPortId(),
                                       pfc_send_pause_msg->getPriority());
      break;
    }
    case isekai::RouterEventType::kCollectPortStatistics: {
      auto port_stats_msg =
          omnetpp::check_and_cast<CollectPortStatisticsMessage*>(msg);
      ports_[port_stats_msg->getPortId()]->CollectPortStatistics();
      if (!finish_called_) {
        scheduleAt(omnetpp::simTime() +
                       omnetpp::SimTime(port_stats_collection_interval_us_,
                                        omnetpp::SIMTIME_US),
                   port_stats_msg);
      }
      break;
    }
    default: {
      LOG(ERROR) << "unknown message type.";
      delete msg;
      break;
    }
  }
}

void NetworkRouter::finish() {
  finish_called_ = true;
  stats_collection_->FlushStatistics();
}
