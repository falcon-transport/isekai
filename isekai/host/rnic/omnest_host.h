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

#ifndef ISEKAI_HOST_RNIC_OMNEST_HOST_H_
#define ISEKAI_HOST_RNIC_OMNEST_HOST_H_

#include <memory>
#include <string>

#include "absl/status/status.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/host/rnic/memory_interface.h"
#include "isekai/host/rnic/omnest_environment.h"
#include "isekai/host/rnic/omnest_packet_builder.h"
#include "isekai/host/rnic/omnest_stats_collection.h"
#include "isekai/host/rnic/rnic.h"
#include "isekai/host/traffic/traffic_generator.h"
#include "omnetpp/cmessage.h"
#include "omnetpp/csimplemodule.h"

// The host class that integrates traffic generator, RNIC, etc. runs in OMNest
// simulation.
class OmnestHost : public isekai::IsekaiHostInterface {
 public:
  // Initializes host id/ip.
  void InitializeHostIdAndIp(const isekai::SimulationConfig& simulation_config);
  // Initializes the statistic collection model in the host.
  void InitializeStatisticCollection(
      isekai::SimulationConfig& simulation_config);
  // Initializes the models in the host.
  void InitializeOmnestEnvironment();
  //
  // InitializeRNIC() will move the ownership of the objects to RNic.
  void InitializeRNIC(
      std::unique_ptr<std::vector<std::unique_ptr<isekai::MemoryInterface>>>
          hif,
      std::unique_ptr<isekai::RdmaBaseInterface> rdma,
      std::unique_ptr<isekai::TrafficShaperInterface> traffic_shaper,
      std::unique_ptr<isekai::FalconInterface> falcon);
  absl::Status InitializePacketBuilder();
  absl::Status InitializeTrafficGenerator(
      isekai::ConnectionManagerInterface* connection_manager,
      const isekai::TrafficPatternConfig& traffic_pattern,
      int falcon_version = 1);
  void GetRdmaConfig(const isekai::SimulationConfig& simulation_config,
                     isekai::RdmaConfig& rdma_config);
  void GetFalconConfig(const isekai::SimulationConfig& simulation_config,
                       isekai::FalconConfig& falcon_config);
  void GetRNicConfig(const isekai::SimulationConfig& simulation_config,
                     isekai::RNicConfig& rnic_config);
  void GetTrafficShaperConfig(
      const isekai::SimulationConfig& simulation_config,
      isekai::TrafficShaperConfig& traffic_shaper_config);
  void set_enable(const isekai::SimulationConfig& simulation_config);
  bool IsEnabled() const { return is_enable_; }

  // getters to return the models in the host.
  isekai::OmnestEnvironment* get_omnest_env() const { return env_.get(); }
  isekai::TrafficGenerator* get_traffic_generator() const {
    return traffic_generator_.get();
  }
  isekai::RNic* get_rnic() const { return rnic_.get(); }
  isekai::OmnestPacketBuilder* get_packet_builder() const {
    return packet_builder_.get();
  }
  const std::string& get_host_id() const { return host_id_; }
  const std::string& get_host_ip() const { return host_ip_; }
  isekai::OmnestStatisticCollection* get_stats_collection() const {
    return stats_collection_.get();
  }

 protected:
  // The default method in OMNest to handle all received messages.
  void handleMessage(omnetpp::cMessage* msg) override;
  // The default method in OMNest to initialize the module (supporting
  // multi-stage initialization).
  void initialize() override {}
  // By default, OMNest only has 1 initialization stage (stage = 0), and the
  // initialize() is called for stage = 0. Find more details about the
  // cSimpleModule initialiation at
  // https://doc.omnetpp.org/omnetpp/api/classomnetpp_1_1cSimpleModule.html.
  void initialize(int stage) override {
    if (stage == 0) {
      initialize();
    }
  }
  int numInitStages() const override { return 1; }
  // Called at the end of the simulation to get scalar statistics.
  void finish() override;

 private:
  void ExecuteEvent(omnetpp::cMessage* msg);

  // The OmnestEnvironment will be used by traffic generator and RNIC.
  // OmnestHost owns OmnestEnvironment, traffic generator, RNIC and
  // OmnestStatisticCollection.
  std::unique_ptr<isekai::OmnestEnvironment> env_;
  std::unique_ptr<isekai::TrafficGenerator> traffic_generator_;
  std::unique_ptr<isekai::RNic> rnic_;
  std::unique_ptr<isekai::OmnestStatisticCollection> stats_collection_;
  std::unique_ptr<isekai::OmnestPacketBuilder> packet_builder_;
  // Stores the host id.
  std::string host_id_ = "";
  // Stores the host ip address.
  std::string host_ip_;
  // True if the host needs to be initialized in the simulation.
  bool is_enable_ = true;
};

#endif  // ISEKAI_HOST_RNIC_OMNEST_HOST_H_
