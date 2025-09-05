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

#include "isekai/host/rnic/omnest_host.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "glog/logging.h"
#include "isekai/common/common_util.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/file_util.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon_histograms.h"
#include "isekai/host/falcon/falcon_histograms.pb.h"
#include "isekai/host/rnic/omnest_environment.h"
#include "isekai/host/rnic/omnest_packet_builder.h"
#include "isekai/host/rnic/omnest_stats_collection.h"
#include "isekai/host/rnic/rnic.h"
#include "isekai/host/traffic/traffic_generator.h"
#include "omnetpp/cdataratechannel.h"
#include "omnetpp/checkandcast.h"
#include "omnetpp/cmessage.h"
#include "omnetpp/csimulation.h"
#include "omnetpp/simtime.h"
#include "omnetpp/simtime_t.h"
#include "omnetpp/simutil.h"

namespace {
bool IsInIncast(
    const isekai::TrafficPatternConfig::IncastTraffic& incast_traffic,
    absl::string_view host_id) {
  if (std::find_if(incast_traffic.sender_host_ids().begin(),
                   incast_traffic.sender_host_ids().end(),
                   [host_id](absl::string_view sender_host_id) {
                     return absl::StartsWith(sender_host_id, host_id);
                   }) != incast_traffic.sender_host_ids().end()) {
    return true;
  }

  if (std::find_if(incast_traffic.victim_host_ids().begin(),
                   incast_traffic.victim_host_ids().end(),
                   [host_id](absl::string_view victim_host_id) {
                     return absl::StartsWith(victim_host_id, host_id);
                   }) != incast_traffic.victim_host_ids().end()) {
    return true;
  }

  return false;
}

bool IsInUniformRandom(const isekai::TrafficPatternConfig::UniformRandomTraffic&
                           uniform_random_traffic,
                       absl::string_view host_id) {
  if (std::find_if(uniform_random_traffic.initiator_host_ids().begin(),
                   uniform_random_traffic.initiator_host_ids().end(),
                   [host_id](absl::string_view initiator_host_id) {
                     return absl::StartsWith(initiator_host_id, host_id);
                   }) != uniform_random_traffic.initiator_host_ids().end()) {
    return true;
  }

  if (std::find_if(uniform_random_traffic.target_host_ids().begin(),
                   uniform_random_traffic.target_host_ids().end(),
                   [host_id](absl::string_view target_host_id) {
                     return absl::StartsWith(target_host_id, host_id);
                   }) != uniform_random_traffic.target_host_ids().end()) {
    return true;
  }

  return false;
}

bool IsInExplicitPattern(
    const isekai::TrafficPatternConfig::ExplicitPattern& explicit_pattern,
    absl::string_view host_id) {
  for (const auto& flow : explicit_pattern.flows()) {
    if (absl::StartsWith(flow.initiator_host_id(), host_id)) {
      return true;
    }
    if (absl::StartsWith(flow.target_host_id(), host_id)) {
      return true;
    }
  }

  return false;
}

bool IsInCompositePattern(
    const isekai::TrafficPatternConfig::CompositePattern& composite_pattern,
    absl::string_view host_id) {
  for (const auto& incast : composite_pattern.incast()) {
    if (IsInIncast(incast, host_id)) {
      return true;
    }
  }
  for (const auto& uniform_random : composite_pattern.uniform_random()) {
    if (IsInUniformRandom(uniform_random, host_id)) {
      return true;
    }
  }
  for (const auto& explicit_pattern : composite_pattern.explicit_pattern()) {
    if (IsInExplicitPattern(explicit_pattern, host_id)) {
      return true;
    }
  }
  return false;
}

}  // namespace

void OmnestHost::InitializeHostIdAndIp(
    const isekai::SimulationConfig& simulation_config) {
  host_id_ = par("host_id").stringValue();
  host_ip_ = par("host_ip").stringValue();
}

void OmnestHost::GetRdmaConfig(
    const isekai::SimulationConfig& simulation_config,
    isekai::RdmaConfig& rdma_config) {
  auto host_config =
      GetHostConfigProfile(host_id_, simulation_config.network());
  if (host_config.has_rdma_configuration()) {
    rdma_config = host_config.rdma_configuration();
  }
}

void OmnestHost::GetFalconConfig(
    const isekai::SimulationConfig& simulation_config,
    isekai::FalconConfig& falcon_config) {
  auto host_config =
      GetHostConfigProfile(host_id_, simulation_config.network());
  if (host_config.has_falcon_configuration()) {
    falcon_config = host_config.falcon_configuration();
  }
}

void OmnestHost::GetRNicConfig(
    const isekai::SimulationConfig& simulation_config,
    isekai::RNicConfig& rnic_config) {
  auto host_config =
      GetHostConfigProfile(host_id_, simulation_config.network());
  if (host_config.has_rnic_configuration()) {
    rnic_config = host_config.rnic_configuration();
  }
}

void OmnestHost::GetTrafficShaperConfig(
    const isekai::SimulationConfig& simulation_config,
    isekai::TrafficShaperConfig& traffic_shaper_config) {
  auto host_config =
      GetHostConfigProfile(host_id_, simulation_config.network());
  if (host_config.has_traffic_shaper_configuration()) {
    traffic_shaper_config = host_config.traffic_shaper_configuration();
  }
}

void OmnestHost::InitializeStatisticCollection(
    isekai::SimulationConfig& simulation_config) {
  stats_collection_ = std::make_unique<isekai::OmnestStatisticCollection>(
      isekai::OmnestStatisticCollection::kHostStats, this,
      simulation_config.stats_collection());
}

void OmnestHost::InitializeOmnestEnvironment() {
  auto host_rng_key = absl::StrCat(host_id_, "@", host_ip_);
  env_ = std::make_unique<isekai::OmnestEnvironment>(this, host_rng_key);
}

void OmnestHost::InitializeRNIC(
    std::unique_ptr<std::vector<std::unique_ptr<isekai::MemoryInterface>>> hif,
    std::unique_ptr<isekai::RdmaBaseInterface> rdma,
    std::unique_ptr<isekai::TrafficShaperInterface> traffic_shaper,
    std::unique_ptr<isekai::FalconInterface> falcon) {
  rnic_ = std::make_unique<isekai::RNic>(std::move(hif), std::move(rdma),
                                         std::move(traffic_shaper),
                                         std::move(falcon));
}

absl::Status OmnestHost::InitializePacketBuilder() {
  if (rnic_ == nullptr)
    return absl::FailedPreconditionError("RNIC is not initialized yet.");
  if (env_ == nullptr)
    return absl::FailedPreconditionError("environment is not initialized yet.");

  // Gets the transmission rate.
  omnetpp::cDatarateChannel* tx_channel =
      omnetpp::check_and_cast_nullable<omnetpp::cDatarateChannel*>(
          gate("out")->findTransmissionChannel());
  CHECK(tx_channel != nullptr) << "Can not find tx channel.";
  packet_builder_ = std::make_unique<isekai::OmnestPacketBuilder>(
      this, rnic_->get_falcon_model(), env_.get(), stats_collection_.get(),
      host_ip_, tx_channel, get_host_id());
  return absl::OkStatus();
}

absl::Status OmnestHost::InitializeTrafficGenerator(
    isekai::ConnectionManagerInterface* connection_manager,
    const isekai::TrafficPatternConfig& traffic_pattern, int falcon_version) {
  if (rnic_ == nullptr)
    return absl::FailedPreconditionError("RNIC is not initialized yet.");
  if (env_ == nullptr)
    return absl::FailedPreconditionError("environment is not initialized yet.");
  if (host_id_.empty())
    return absl::FailedPreconditionError("hostId is not initialized yet.");
  if (stats_collection_ == nullptr)
    return absl::FailedPreconditionError(
        "statistic collection model is not initialized yet.");

  traffic_generator_ = std::make_unique<isekai::TrafficGenerator>(
      connection_manager, rnic_->get_rdma_model(), env_.get(),
      stats_collection_.get(), host_id_, traffic_pattern, falcon_version);
  return absl::OkStatus();
}

void OmnestHost::handleMessage(omnetpp::cMessage* msg) {
  // Received a RNIC event scheduling msg.
  if (dynamic_cast<isekai::CallbackMessage*>(msg)) {
    ExecuteEvent(msg);
  } else {
    VLOG(2) << msg->getArrivalModule()->getFullPath()
            << " receives packet from " << msg->getSenderModule()->getFullPath()
            << " at time: " << SIMTIME_STR(omnetpp::simTime());
    packet_builder_->ExtractAndHandleTransportPacket(msg);
  }
}

void OmnestHost::ExecuteEvent(omnetpp::cMessage* msg) {
  auto* call_back_message =
      omnetpp::check_and_cast<isekai::CallbackMessage*>(msg);
  // Get a reference to the callback in msg.
  absl::AnyInvocable<void()>& callback =
      call_back_message->get_call_back_function();
  // Runs the callback to perform the scheduled event.
  callback();
  // Deallocates the self message by the host. Delete after callback() because
  // the callback is a reference to a field in msg.
  delete msg;
}

void OmnestHost::set_enable(const isekai::SimulationConfig& simulation_config) {
  if (!simulation_config.has_traffic_pattern()) {
    is_enable_ = false;
    return;
  }

  if (simulation_config.traffic_pattern().has_incast()) {
    // Checks if host is in incast.
    is_enable_ =
        IsInIncast(simulation_config.traffic_pattern().incast(), host_id_);
  } else if (simulation_config.traffic_pattern().has_uniform_random()) {
    // Checks if host is in uniform random
    is_enable_ = IsInUniformRandom(
        simulation_config.traffic_pattern().uniform_random(), host_id_);
  } else if (simulation_config.traffic_pattern().has_explicit_pattern()) {
    // Checks if host is in explicit pattern
    is_enable_ = IsInExplicitPattern(
        simulation_config.traffic_pattern().explicit_pattern(), host_id_);
  } else {
    // Checks if host is in composite traffic pattern
    is_enable_ = IsInCompositePattern(
        simulation_config.traffic_pattern().composite_pattern(), host_id_);
  }
}

void OmnestHost::finish() {
  if (!is_enable_) {
    return;
  }
  if (stats_collection_) {
    stats_collection_->FlushStatistics();
  }
  // If this was a FALCON simulation, and a results directory is specified, only
  // then output the histograms to a file.
  const char* result_dir =
      omnetpp::getEnvir()->getConfig()->getConfigValue("result-dir");
  if (result_dir == nullptr) {
    return;
  }
  // Write out FALCON latency statistics in per-host files.
  if (rnic_->get_falcon_model()) {
    isekai::FalconHistograms histograms;
    histograms.set_host_id(host_id_);
    rnic_->get_falcon_model()
        ->get_histogram_collector()
        ->DumpAllHistogramsToProto(&histograms);
    std::string file_path =
        absl::StrCat(result_dir, "/", host_id_, "_falcon_latency_histograms");
    CHECK_OK(isekai::WriteProtoToFile(file_path, histograms));
  }
  // Write out RDMA latency statistics in per-host files.
  if (rnic_->get_rdma_model()) {
    if (stats_collection_->GetConfig().has_rdma_flags() &&
        stats_collection_->GetConfig().rdma_flags().enable_histograms()) {
      isekai::RdmaLatencyHistograms histograms;
      histograms.set_host_id(host_id_);
      rnic_->get_rdma_model()->DumpLatencyHistogramsToProto(&histograms);
      std::string file_path =
          absl::StrCat(result_dir, "/", host_id_, "_rdma_latency_histograms");
      CHECK_OK(isekai::WriteProtoToFile(file_path, histograms));
    }
  }
}
