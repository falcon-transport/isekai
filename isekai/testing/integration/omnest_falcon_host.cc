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

#include "isekai/testing/integration/omnest_falcon_host.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "glog/logging.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/file_util.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen2/falcon_model.h"
#include "isekai/host/falcon/gen3/falcon_model.h"
#include "isekai/host/rdma/rdma_falcon_model.h"
#include "isekai/host/rnic/connection_manager.h"
#include "isekai/host/rnic/memory_interface.h"
#include "isekai/host/rnic/traffic_shaper_model.h"
#include "isekai/host/traffic/traffic_generator.h"
#include "omnetpp/regmacros.h"

// Registers FalconHost module into OMNest.
// This macro prevents putting the code input namespace isekai.
Define_Module(FalconHost);

void FalconHost::initialize(int stage) {
  if (stage == 0) {
    LOG(INFO) << "Init FALCON components (stage 0).";
    initialize();
    return;
  }

  if (stage == 1) {
    if (!IsEnabled()) {
      return;
    }

    LOG(INFO) << "Init RDMA application (stage 1).";
    isekai::SimulationConfig simulation;
    std::string config_file =
        getSystemModule()->par("simulation_config").stringValue();
    CHECK_OK(isekai::ReadTextProtoFromFile(config_file, &simulation));

    int falcon_version = 1;
    isekai::FalconConfig falcon_configuration;
    GetFalconConfig(simulation, falcon_configuration);
    if (falcon_configuration.has_version()) {
      falcon_version = falcon_configuration.version();
    }
    // Initialize Falcon with the right version if it is specified in the
    // config, otherwise default to version 1.
    falcon_configuration =
        isekai::DefaultConfigGenerator::DefaultFalconConfig(falcon_version);
    GetFalconConfig(simulation, falcon_configuration);

    isekai::ConnectionManager* connection_manager =
        isekai::ConnectionManager::GetConnectionManager();

    if (InitializeTrafficGenerator(connection_manager,
                                   simulation.traffic_pattern(), falcon_version)
            .ok()) {
      LOG(INFO) << get_host_id() << " creates traffic generator.";
    }
    get_traffic_generator()->GenerateRdmaOp();
    LOG(INFO) << get_host_id() << " starts traffic generation.";
    return;
  }

  LOG(FATAL) << "unsupported initialization stage!";
}

void FalconHost::initialize() {
  // Loads the host configurations.
  isekai::SimulationConfig simulation;
  std::string simulation_config_file =
      getSystemModule()->par("simulation_config").stringValue();
  CHECK(isekai::ReadTextProtoFromFile(simulation_config_file, &simulation).ok())
      << "fails to load simulation config proto text.";

  absl::Status config_validation = ValidateSimulationConfig(simulation);
  if (!config_validation.ok()) {
    LOG(FATAL) << "Invalid simulation configuration: "
               << config_validation.message();
  }
  // Loads the host id/ip information.
  InitializeHostIdAndIp(simulation);
  LOG(INFO) << get_host_id() << " has index: " << this->getIndex();

  // Checks if the host should be initialized or not.
  set_enable(simulation);
  if (!IsEnabled()) {
    return;
  }

  InitializeOmnestEnvironment();
  if (get_omnest_env() != nullptr) {
    LOG(INFO) << get_host_id() << " creates omnest env.";
  }
  InitializeStatisticCollection(simulation);

  // Gets all components' configs
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  GetRNicConfig(simulation, rnic_config);

  isekai::TrafficShaperConfig traffic_shaper_configuration =
      isekai::DefaultConfigGenerator::DefaultTrafficShaperConfig();
  GetTrafficShaperConfig(simulation, traffic_shaper_configuration);

  isekai::RdmaConfig rdma_options =
      isekai::DefaultConfigGenerator::DefaultRdmaConfig();
  GetRdmaConfig(simulation, rdma_options);

  int falcon_version = 1;
  isekai::FalconConfig falcon_configuration;
  GetFalconConfig(simulation, falcon_configuration);
  if (falcon_configuration.has_version()) {
    falcon_version = falcon_configuration.version();
  }
  // Initialize Falcon with the right version if it is specified in the config,
  // otherwise default to version 1.
  falcon_configuration =
      isekai::DefaultConfigGenerator::DefaultFalconConfig(falcon_version);
  GetFalconConfig(simulation, falcon_configuration);

  // Initializes connection manager with the simulation network configurations.
  isekai::ConnectionManager* connection_manager =
      isekai::ConnectionManager::GetConnectionManager();

  connection_manager->PopulateHostInfo(simulation.network());

  // Initializes the Host interface.
  CHECK(rnic_config.host_interface_config_size() >= 1)
      << "At least one hif should be there";
  auto host_interface_list =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif = std::make_unique<isekai::MemoryInterface>(host_intf_config,
                                                         get_omnest_env());
    host_interface_list->push_back(std::move(hif));
  }

  // Initializes the traffic shaper.
  auto traffic_shaper = std::make_unique<isekai::TrafficShaperModel>(
      traffic_shaper_configuration, get_omnest_env(), get_stats_collection());

  // Initializes the RDMA model.
  auto rdma = std::make_unique<isekai::RdmaFalconModel>(
      rdma_options, get_omnest_env(), get_stats_collection(),
      connection_manager);

  std::unique_ptr<isekai::FalconInterface> falcon;
  if (falcon_configuration.version() == 1) {
    falcon = std::make_unique<isekai::FalconModel>(
        falcon_configuration, get_omnest_env(), get_stats_collection(),
        connection_manager, get_host_id(),
        rnic_config.host_interface_config_size());
  } else if (falcon_configuration.version() == 2) {
    falcon = std::make_unique<isekai::Gen2FalconModel>(
        falcon_configuration, get_omnest_env(), get_stats_collection(),
        connection_manager, get_host_id(),
        rnic_config.host_interface_config_size());
  } else if (falcon_configuration.version() == 3) {
    falcon = std::make_unique<isekai::Gen3FalconModel>(
        falcon_configuration, get_omnest_env(), get_stats_collection(),
        connection_manager, get_host_id(),
        rnic_config.host_interface_config_size());
  } else {
    LOG(FATAL) << "Unsupported Falcon version: "
               << falcon_configuration.version();
  }

  // Initializes the RNIC.
  InitializeRNIC(std::move(host_interface_list), std::move(rdma),
                 std::move(traffic_shaper), std::move(falcon));
  if (get_rnic()->get_rdma_model() != nullptr) {
    LOG(INFO) << get_host_id() << " creates RDMA.";
  }
  if (get_rnic()->get_traffic_shaper() != nullptr) {
    LOG(INFO) << get_host_id() << " creates Traffic Shaper.";
  }
  if (InitializePacketBuilder().ok()) {
    LOG(INFO) << get_host_id() << " creates Packet Builder.";
  }

  // Connects traffic shaper to packet builder.
  get_rnic()->get_traffic_shaper()->ConnectPacketBuilder(get_packet_builder());
  // Connects RDMA to FALCON.
  static_cast<isekai::RdmaFalconModel*>(get_rnic()->get_rdma_model())
      ->ConnectFalcon(get_rnic()->get_falcon_model());
  // Connects RDMA to HIF
  static_cast<isekai::RdmaFalconModel*>(get_rnic()->get_rdma_model())
      ->ConnectHostInterface(get_rnic()->get_host_interface_list());
  // Connects FALCON to traffic shaper and RDMA.
  static_cast<isekai::FalconModel*>(get_rnic()->get_falcon_model())
      ->ConnectShaper(get_rnic()->get_traffic_shaper());
  static_cast<isekai::FalconModel*>(get_rnic()->get_falcon_model())
      ->ConnectRdma(
          static_cast<isekai::RdmaFalconModel*>(get_rnic()->get_rdma_model()));
  // Registers to connection manager.
  get_rnic()->RegisterToConnectionManager(get_host_id(), connection_manager);
}

// Validates simulation configurations.
absl::Status FalconHost::ValidateSimulationConfig(
    const isekai::SimulationConfig& config) {
  // Tx scheduler_pipeline_delay_in_cycles should be less or equal than any
  // field in ulp_xoff_thresholds.
  // Reason: when Falcon xoff is triggered, any packets stuck at RDMA -> Falcon
  // pipeline should still be accepted by Falcon after the pipeline delay.
  for (const auto& host_config : config.network().host_configs()) {
    if (!host_config.falcon_configuration().has_ulp_xoff_thresholds() ||
        host_config.rdma_configuration().scheduler_pipeline_delay_in_cycles() ==
            0) {
      continue;
    }
    uint64_t pipeline_delay =
        host_config.rdma_configuration().scheduler_pipeline_delay_in_cycles();
    auto xoff_thresholds =
        host_config.falcon_configuration().ulp_xoff_thresholds();
    if (xoff_thresholds.tx_packet_request() > 0 &&
        xoff_thresholds.tx_packet_request() < pipeline_delay) {
      return absl::InvalidArgumentError(
          "Xoff Tx packet request threshold should be no less than scheduler "
          "pipeline delay.");
    }
    if (xoff_thresholds.tx_buffer_request() > 0 &&
        xoff_thresholds.tx_buffer_request() < pipeline_delay) {
      return absl::InvalidArgumentError(
          "Xoff Tx buffer request threshold should be no less than scheduler "
          "pipeline delay.");
    }
    if (xoff_thresholds.tx_packet_data() > 0 &&
        xoff_thresholds.tx_packet_data() < pipeline_delay) {
      return absl::InvalidArgumentError(
          "Xoff Tx packet data threshold should be no less than scheduler "
          "pipeline delay.");
    }
    if (xoff_thresholds.tx_buffer_data() > 0 &&
        xoff_thresholds.tx_buffer_data() < pipeline_delay) {
      return absl::InvalidArgumentError(
          "Xoff Tx buffer data threshold should be no less than scheduler "
          "pipeline delay.");
    }
    if (xoff_thresholds.rx_packet_request() > 0 &&
        xoff_thresholds.rx_packet_request() < pipeline_delay) {
      return absl::InvalidArgumentError(
          "Xoff Rx packet request threshold should be no less than scheduler "
          "pipeline delay.");
    }
    if (xoff_thresholds.rx_buffer_request() > 0 &&
        xoff_thresholds.rx_buffer_request() < pipeline_delay) {
      return absl::InvalidArgumentError(
          "Xoff Rx buffer request threshold should be no less than scheduler "
          "pipeline delay.");
    }
  }
  return absl::OkStatus();
}
