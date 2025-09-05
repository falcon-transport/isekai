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

#ifndef ISEKAI_FABRIC_NETWORK_ROUTER_H_
#define ISEKAI_FABRIC_NETWORK_ROUTER_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/constants.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/fabric/memory_management_config.pb.h"
#include "isekai/fabric/model_interfaces.h"
#include "isekai/fabric/network_port_stats_m.h"
#include "isekai/fabric/output_schedule_message_m.h"
#include "isekai/fabric/pfc_message_m.h"
#include "isekai/host/rnic/omnest_stats_collection.h"
#include "omnetpp/cdataratechannel.h"
#include "omnetpp/cmessage.h"
#include "omnetpp/simtime_t.h"

// The class implements the network router as a cSimpleModule in OMNest.
class NetworkRouter : public isekai::NetworkRouterInterface {
 public:
  ~NetworkRouter() override;
  // Called by the input port logic in requesting the packet to be routed across
  // the router.
  void RoutePacket(omnetpp::cMessage* packet) override;
  // Called by the routing pipeline to request the packet to be enqueued in an
  // output queue.
  void EnqueuePacket(omnetpp::cMessage* packet, int port_id) override;
  // Called by the output port to send the packet out to the network.
  void SendPacket(omnetpp::cMessage* packet, int port_id,
                  omnetpp::cDatarateChannel* transmission_channel) override;
  // Schedule an event to ask port[port_id] to send out packets to the network.
  void ScheduleOutput(int port_id, const omnetpp::simtime_t& delay) override;
  // Schedule an event to set PFC pause.
  void SetPfcPaused(PfcMessage* pfc_pause_msg,
                    const omnetpp::simtime_t& start_pfc_pause_delay) override;

  isekai::MemoryManagementUnitInterface* GetMemoryManagementUnit()
      const override {
    return memory_management_unit_.get();
  }
  isekai::PortInterface* GetPort(int port_id) const override {
    return ports_[port_id].get();
  }
  bool IsPortConnected(int port_id) const override {
    return (gate("port$o", port_id)->isConnected() &&
            gate("port$i", port_id)->isConnected());
  }
  uint32_t GetMtuSize() const override { return mtu_size_; }
  int GetRouterStage() const override { return router_stage_; }

 protected:
  // This function receives and interprets all events and passes them to the
  // right subcomponents.
  void handleMessage(omnetpp::cMessage* msg) override;
  // The default function in OMNest to initialize a class. See the basic usage
  // of initialize() method of cComponent here:
  // https://doc.omnetpp.org/omnetpp/manual/#sec:simple-modules:initialize-and-finish.
  // In initialize(), we construct ports, MMU and routing pipeline based on the
  // given router parameters in NED file.
  void initialize() override;
  // Called at the end of the simulation to get scalar statistics.
  void finish() override;

 private:
  // Initializes memory management unit. The MMU holds a pointer to the router.
  void InitializeMemoryManagementUnit();
  // Initializes routing pipeline module. The routing pipeline holds a pointer
  // to the router.
  void InitializeRoutingPipeline(absl::string_view routing_table_path);
  // Initializes all the ports of the router. Each port holds a pointer to the
  // router.
  void InitializePorts();
  void InitializeStatsCollection(isekai::SimulationConfig simulation_config);

  // Gets the port id of the received packet.
  int GetArrivalPortId(const omnetpp::cMessage* packet) {
    return packet->getArrivalGate()->getIndex();
  }

  // The router owns MMU, routing pipeline and ports.
  std::unique_ptr<isekai::MemoryManagementUnitInterface>
      memory_management_unit_;
  std::unique_ptr<isekai::RoutingPipelineInterface> routing_pipeline_;
  std::vector<std::unique_ptr<isekai::PortInterface>> ports_;
  // The router owns the statistic collection so that other router components
  // can records results via stats_collection_.
  std::unique_ptr<isekai::OmnestStatisticCollection> stats_collection_;
  // For each port, it has its own OutputScheduleMessage, whose kind value is 2
  // (RouterEventType::kOutputScheduling).
  std::vector<OutputScheduleMessage*> output_schedule_messages_;
  // Used to trigger periodical port statistics collection.
  std::vector<CollectPortStatisticsMessage*> clct_port_stats_messages_;
  std::string router_name_;
  // The stage value 1/2/3 maps to S1/S2/S3, respectively.
  int router_stage_;
  omnetpp::simtime_t router_delay_;
  // Required configuration from RouterConfigProfile.

  // Required configuration from RouterConfigProfile.
  // The number of TX queues per port.
  uint32_t tx_queue_num_;
  // Memory management unit configuration.
  isekai::MemoryManagementUnitConfig mmu_config_;

  // Optional configuration from RouterConfigProfile with default values
  // specified below.
  static isekai::StatisticsCollectionConfig::RouterFlags
      stats_collection_flags_;
  bool enable_port_stats_collection_ = false;
  // The egress port statistics collection interval.
  uint32_t port_stats_collection_interval_us_ = 5U;
  // Max MTU supported by the network. Any packet larger than this will throw an
  // error during the simulation.
  uint32_t mtu_size_ = isekai::kNetworkMtu;
  // The routing pipeline delay in ns.
  uint32_t routing_pipeline_delay_ns_ = 600;
  // The routing scheme used in routing pipeline.
  isekai::RouterConfigProfile::PortSelectionPolicy port_selection_policy_ =
      isekai::RouterConfigProfile::WCMP;
  // The arbitration scheme for TX packets.
  isekai::RouterConfigProfile::ArbitrationScheme arbitration_scheme_ =
      isekai::RouterConfigProfile::FIXED_PRIORITY;
  // Enabler of per-flow round robin.
  bool enable_per_flow_round_robin_ = false;
  std::atomic<bool> finish_called_ = false;
};

#endif  // ISEKAI_FABRIC_NETWORK_ROUTER_H_
