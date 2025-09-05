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

#include "isekai/host/traffic/traffic_generator.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"
#include "absl/functional/bind_front.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "absl/time/time.h"
#include "glog/logging.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/status_util.h"

namespace isekai {
namespace {

// Collects scalar and vector offered load between a src and dst host pair.
// Flags: enable_vector_offered_load, enable_scalar_offered_load
constexpr std::string_view kStatVectorOfferedLoad =
    "traffic_generator.offered_load_vector.src_$0.dst_$1";
constexpr std::string_view kStatScalarOfferedLoad =
    "traffic_generator.offered_load_scalar.src_$0.dst_$1";

// Collects scalar average of inter op scheduling interval.
// Flags: enable_op_schedule_interval
constexpr std::string_view kStatScalarOpScheduleInterval =
    "traffic_generator.op_schedule_interval";

// Collects scalar goodput, completed ops and mean op_latency.
// Flags: enable_scalar_op_stats
constexpr std::string_view kStatScalarGoodput =
    "traffic_generator.goodput.src_$0.dst_$1";
constexpr std::string_view kStatScalarCompletedOps =
    "traffic_generator.completed_ops_scalar.src_$0.dst_$1";
constexpr std::string_view kStatScalarOpLatency =
    "traffic_generator.op_latency_scalar.src_$0.dst_$1.pattern_$2.opcode_$3."
    "size_$4";

// Collects vector generated op size, completed ops, op size and latency.
// Flags: enable_vector_op_stats
constexpr std::string_view kStatVectorGeneratedOpSize =
    "traffic_generator.generated_op_size.pattern_$0.opcode_$1";
constexpr std::string_view kStatVectorCompletedOpSize =
    "traffic_generator.completed_op_size.src_$0.dst_$1.pattern_$2.opcode_$3."
    "size_$4";
constexpr std::string_view kStatVectorCompletedOps =
    "traffic_generator.completed_ops.src_$0.dst_$1.pattern_$2.opcode_$3."
    "size_$4";
constexpr std::string_view kStatVectorOpLatency =
    "traffic_generator.op_latency.src_$0.dst_$1.pattern_$2.opcode_$3.size_$4";

// Collects vector tx/rx bytes between a local and remote qp.
// Flags: enable_per_qp_tx_rx_bytes
constexpr std::string_view kStatVectorTxBytesPerQp =
    "traffic_generator.tx_bytes.local_qp_$0.remote_qp_$1";
constexpr std::string_view kStatVectorRxBytesPerQp =
    "traffic_generator.rx_bytes.local_qp_$0.remote_qp_$1";

constexpr std::string_view kReadOpName = "READ";
constexpr std::string_view kWriteOpName = "WRITE";
constexpr std::string_view kSendOpName = "SEND";
constexpr std::string_view kReceiveOpName = "RECV";

// The host mentioned in traffic generator config follows this pattern:
// "hostId_bifurcationId". This function splits the host id and bifurcation id
// from the provided host. The maximum number of bifurcated supported by something
// NIC is 4, that's we use uint8_t here.
std::pair<std::string, uint8_t> SplitHostIdAndBifurcationId(
    const std::string& full_id) {
  std::string host_id;
  std::string bifurcated_host_id;
  auto found = full_id.find_last_of('_');
  CHECK(found != std::string::npos) << "Bifurcation id is not specified";
  host_id = full_id.substr(0, found);
  bifurcated_host_id = full_id.substr(found + 1);
  CHECK(!bifurcated_host_id.empty()) << "Bifurcation id is empty";
  return std::make_pair(host_id, std::stoi(bifurcated_host_id));
}

double Exponential(std::mt19937* rng, double mean) {
  std::exponential_distribution<> dist(1.0 / mean);
  return dist(*rng);
}

double Gaussian(std::mt19937* rng, double mean, double std) {
  std::normal_distribution<> dist(mean, std);
  return dist(*rng);
}

// Return a random double value between [min, max).
double DoubleUniform(std::mt19937* rng, double min, double max) {
  std::uniform_real_distribution<> dist(min, max);
  return dist(*rng);
}

// Return a random int value between [min, max).
int32_t IntUniform(std::mt19937* rng, int32_t min, int32_t max) {
  CHECK_LT(min, max);
  std::uniform_int_distribution<> dist(min, max - 1);
  return dist(*rng);
}

absl::string_view ConvertOpCodeToString(const RdmaOpcode& op_code) {
  switch (op_code) {
    case RdmaOpcode::kRead:
      return kReadOpName;
    case RdmaOpcode::kRecv:
      return kReceiveOpName;
    case RdmaOpcode::kSend:
      return kSendOpName;
    case RdmaOpcode::kWrite:
      return kWriteOpName;
  }
}

}  // namespace

StatisticsCollectionConfig::TrafficGeneratorFlags
    TrafficGenerator::stats_collection_flags_;

TrafficGenerator::TrafficGenerator(
    ConnectionManagerInterface* connection_manager, RdmaBaseInterface* rdma,
    Environment* env, StatisticCollectionInterface* stats_collector,
    absl::string_view host_id, const TrafficPatternConfig& traffic_pattern,
    int falcon_version)
    : host_id_(host_id),
      connection_manager_(connection_manager),
      env_(env),
      rdma_(rdma),
      stats_collection_(stats_collector),
      falcon_version_(falcon_version) {
  // Seeds rng_ by the module's host_id. By doing so, we are sure the results
  // are reproducible.
  std::seed_seq seed(host_id_.begin(), host_id_.end());
  rng_ = std::make_unique<std::mt19937>(seed);
  if (stats_collection_ != nullptr &&
      stats_collection_->GetConfig().has_traffic_generator_flags()) {
    stats_collection_flags_ =
        stats_collection_->GetConfig().traffic_generator_flags();
  } else {
    stats_collection_flags_ =
        DefaultConfigGenerator::DefaultTrafficGeneratorStatsFlags();
  }
  ParseConfig(traffic_pattern);
}

void TrafficGenerator::SetDefaultValuesInConnectionConfig(
    TrafficCharacteristics::ConnectionConfig& config) {
  if (!config.has_max_generated_packets()) {
    config.set_max_generated_packets(-1);
  }
  if (!config.has_op_config()) {
    config.mutable_op_config()->set_write_ratio(0.25);
    config.mutable_op_config()->set_read_ratio(0.25);
    config.mutable_op_config()->set_send_ratio(0.25);
    config.mutable_op_config()->set_recv_ratio(0.25);
  }
}

void TrafficGenerator::ParseConfig(
    const TrafficPatternConfig& traffic_pattern) {
  if (traffic_pattern.has_incast()) {
    InitIncast(traffic_pattern.incast(), 0);
  } else if (traffic_pattern.has_uniform_random()) {
    InitUniformRandom(traffic_pattern.uniform_random(), 0);
  } else if (traffic_pattern.has_explicit_pattern()) {
    InitExplicitPattern(traffic_pattern.explicit_pattern(), 0);
  } else {
    InitCompositePattern(traffic_pattern.composite_pattern());
  }
}

void TrafficGenerator::InitIncastWrapper(
    const TrafficPatternConfig_IncastTraffic& incast_traffic, int index,
    TrafficCharacteristics::RdmaOp op_code) {
  IncastInfo traffic_pattern_info_template{};
  traffic_pattern_info_template.traffic_pattern_type =
      TrafficPatternType::kIncast;
  traffic_pattern_info_template.is_initiator = false;

  const ::google::protobuf::RepeatedPtrField<std::string>* initiators;
  const ::google::protobuf::RepeatedPtrField<std::string>* targets;
  if (op_code == TrafficCharacteristics::READ) {
    // If it is READ, then the victim is the initiator.
    initiators = &incast_traffic.victim_host_ids();
    targets = &incast_traffic.sender_host_ids();
  } else {
    // If it is WRITE, then the senders are the initiators.
    initiators = &incast_traffic.sender_host_ids();
    targets = &incast_traffic.victim_host_ids();
  }

  auto initiator_iterator =
      std::find_if(initiators->begin(), initiators->end(),
                   [&](absl::string_view initiator_id) {
                     return absl::StartsWith(initiator_id, host_id_);
                   });

  if (initiator_iterator != initiators->end()) {
    auto initiator_bifurcated_ids =
        SplitHostIdAndBifurcationId(*initiator_iterator);
    traffic_pattern_info_template.is_initiator = true;
    traffic_pattern_info_template.traffic_pattern_id =
        absl::StrCat("incast_", index);
    traffic_pattern_info_template.traffic_characteristics.connection_type =
        incast_traffic.traffic_characteristics().conn_type();
    traffic_pattern_info_template.traffic_characteristics.inline_message =
        incast_traffic.traffic_characteristics().inline_message();
    if (incast_traffic.traffic_characteristics().has_degree_of_multipathing()) {
      traffic_pattern_info_template.traffic_characteristics
          .degree_of_multipathing =
          incast_traffic.traffic_characteristics().degree_of_multipathing();
    }
    // Records the set of initiators in the incast.
    traffic_pattern_info_template.incast_initiators.CopyFrom(*initiators);
    std::sort(traffic_pattern_info_template.incast_initiators.begin(),
              traffic_pattern_info_template.incast_initiators.end());
    // Records the set of targets in the incast.
    traffic_pattern_info_template.incast_targets.CopyFrom(*targets);
    std::sort(traffic_pattern_info_template.incast_targets.begin(),
              traffic_pattern_info_template.incast_targets.end());
    // Updates the incast degree.
    if (incast_traffic.has_incast_degree()) {
      traffic_pattern_info_template.incast_degree =
          incast_traffic.incast_degree();
    }
    if (op_code == TrafficCharacteristics::READ) {
      if (traffic_pattern_info_template.incast_degree >
          traffic_pattern_info_template.incast_targets.size()) {
        traffic_pattern_info_template.incast_degree =
            traffic_pattern_info_template.incast_targets.size();
      }
    } else {
      if (traffic_pattern_info_template.incast_degree >
          traffic_pattern_info_template.incast_initiators.size()) {
        traffic_pattern_info_template.incast_degree =
            traffic_pattern_info_template.incast_initiators.size();
      }
    }
    // Creates the connections.
    for (const auto& target : *targets) {
      auto target_bifurcated_ids = SplitHostIdAndBifurcationId(target);
      if (target_bifurcated_ids.first != host_id_) {
        CHECK_OK(AddFlow(target, target_bifurcated_ids.first,
                         initiator_bifurcated_ids.second,
                         target_bifurcated_ids.second,
                         &traffic_pattern_info_template,
                         /*bidirectional=*/false))
            << "fail to add a flow.";
      }
    }
  }

  if (traffic_pattern_info_template.is_initiator) {
    for (const auto& config :
         incast_traffic.traffic_characteristics().conn_config()) {
      if (config.op_code() == op_code) {
        IncastInfo traffic_pattern_info_per_config(
            traffic_pattern_info_template);
        traffic_pattern_info_per_config.traffic_characteristics
            .connection_config = config;
        SetDefaultValuesInConnectionConfig(
            traffic_pattern_info_per_config.traffic_characteristics
                .connection_config);
        traffic_pattern_info_per_config.traffic_characteristics.offered_load =
            incast_traffic.traffic_characteristics().offered_load() *
            config.offered_load_ratio();
        traffic_pattern_info_per_config.traffic_characteristics.op_config =
            config.op_config();
        double mean = config.size_distribution_params().mean();
        if (config.msg_size_distribution() ==
            TrafficCharacteristics::CUSTOM_SIZE_DIST) {
          // Since each quantile has equal probablility, the mean can be
          // calculated by adding each quantile's size and divide it by the
          // number of quantiles. The mean is then used to calculate average
          // inter-op gap.
          mean =
              std::accumulate(
                  config.size_distribution_params().cdf().quantiles().begin(),
                  config.size_distribution_params().cdf().quantiles().end(),
                  0.0) /
              config.size_distribution_params().cdf().quantiles().size();
        }
        traffic_pattern_info_per_config.avg_interval =
            1e-9 * mean * 8 /
            traffic_pattern_info_per_config.traffic_characteristics
                .offered_load;
        // set the traffic start time
        if (incast_traffic.traffic_characteristics()
                .has_traffic_start_time_ns()) {
          traffic_pattern_info_per_config.traffic_start_time =
              incast_traffic.traffic_characteristics().traffic_start_time_ns() /
              1e9;
        }
        traffic_pattern_info_.push_back(traffic_pattern_info_per_config);
      }
    }
  }
}

void TrafficGenerator::InitIncast(
    const TrafficPatternConfig_IncastTraffic& incast_traffic, int index) {
  InitIncastWrapper(incast_traffic, index, TrafficCharacteristics::READ);
  InitIncastWrapper(incast_traffic, index, TrafficCharacteristics::WRITE);
}

void TrafficGenerator::InitUniformRandom(
    const TrafficPatternConfig_UniformRandomTraffic& uniform_random_traffic,
    int index) {
  UniformRandomInfo traffic_pattern_info_template{};
  traffic_pattern_info_template.traffic_pattern_type =
      TrafficPatternType::kUniformRandom;
  traffic_pattern_info_template.is_initiator = false;

  for (const auto& local_host : uniform_random_traffic.initiator_host_ids()) {
    auto local_bifurcated_ids = SplitHostIdAndBifurcationId(local_host);
    if (host_id_ == local_bifurcated_ids.first) {
      if (traffic_pattern_info_template.is_initiator == false) {
        traffic_pattern_info_template.is_initiator = true;
        traffic_pattern_info_template.traffic_pattern_id =
            absl::StrCat("uniform_random_", index);
        traffic_pattern_info_template.traffic_characteristics.connection_type =
            uniform_random_traffic.traffic_characteristics().conn_type();
        traffic_pattern_info_template.traffic_characteristics.inline_message =
            uniform_random_traffic.traffic_characteristics().inline_message();
        if (uniform_random_traffic.traffic_characteristics()
                .has_degree_of_multipathing()) {
          traffic_pattern_info_template.traffic_characteristics
              .degree_of_multipathing =
              uniform_random_traffic.traffic_characteristics()
                  .degree_of_multipathing();
        }
      }
      // Default to not bidirectional if bidirectional is not specified in the
      // config.
      bool bidirectional = uniform_random_traffic.has_bidirectional()
                               ? uniform_random_traffic.bidirectional()
                               : false;
      for (const auto& remote_host : uniform_random_traffic.target_host_ids()) {
        auto remote_bifurcated_ids = SplitHostIdAndBifurcationId(remote_host);
        if (remote_bifurcated_ids.first != host_id_) {
          CHECK_OK(AddFlow(remote_host, remote_bifurcated_ids.first,
                           local_bifurcated_ids.second,
                           remote_bifurcated_ids.second,
                           &traffic_pattern_info_template,
                           /*bidirectional=*/bidirectional))
              << "fail to add a flow.";
        }
      }
    }
  }

  if (traffic_pattern_info_template.is_initiator) {
    for (const auto& config :
         uniform_random_traffic.traffic_characteristics().conn_config()) {
      UniformRandomInfo traffic_pattern_info_per_config =
          traffic_pattern_info_template;
      traffic_pattern_info_per_config.traffic_characteristics
          .connection_config = config;
      SetDefaultValuesInConnectionConfig(
          traffic_pattern_info_per_config.traffic_characteristics
              .connection_config);
      traffic_pattern_info_per_config.traffic_characteristics.offered_load =
          uniform_random_traffic.traffic_characteristics().offered_load() *
          config.offered_load_ratio();
      traffic_pattern_info_per_config.traffic_characteristics.op_config =
          config.op_config();
      double mean = config.size_distribution_params().mean();
      if (config.msg_size_distribution() ==
          TrafficCharacteristics::CUSTOM_SIZE_DIST) {
        // Getting expected value from CDF
        mean = std::accumulate(
                   config.size_distribution_params().cdf().quantiles().begin(),
                   config.size_distribution_params().cdf().quantiles().end(),
                   0.0) /
               config.size_distribution_params().cdf().quantiles().size();
      }
      traffic_pattern_info_per_config.avg_interval =
          1e-9 * mean * 8 /
          traffic_pattern_info_per_config.traffic_characteristics.offered_load;
      // set the traffic start time
      if (uniform_random_traffic.traffic_characteristics()
              .has_traffic_start_time_ns()) {
        traffic_pattern_info_per_config.traffic_start_time =
            uniform_random_traffic.traffic_characteristics()
                .traffic_start_time_ns() /
            1e9;
      }
      traffic_pattern_info_.push_back(traffic_pattern_info_per_config);
    }
  }
}

void TrafficGenerator::InitExplicitPattern(
    const TrafficPatternConfig_ExplicitPattern& explicit_pattern, int index) {
  ExplicitPatternInfo traffic_pattern_info_template{};
  traffic_pattern_info_template.traffic_pattern_type =
      TrafficPatternType::kExplicitPattern;
  traffic_pattern_info_template.is_initiator = false;

  for (const auto& flow : explicit_pattern.flows()) {
    auto initiator_bifurcated_ids =
        SplitHostIdAndBifurcationId(flow.initiator_host_id());
    if (initiator_bifurcated_ids.first == host_id_) {
      if (traffic_pattern_info_template.is_initiator == false) {
        traffic_pattern_info_template.is_initiator = true;
        traffic_pattern_info_template.traffic_pattern_id =
            absl::StrCat("explicit_pattern_", index);
        traffic_pattern_info_template.traffic_characteristics.connection_type =
            explicit_pattern.traffic_characteristics().conn_type();
        traffic_pattern_info_template.traffic_characteristics.inline_message =
            explicit_pattern.traffic_characteristics().inline_message();
        if (explicit_pattern.traffic_characteristics()
                .has_degree_of_multipathing()) {
          traffic_pattern_info_template.traffic_characteristics
              .degree_of_multipathing =
              explicit_pattern.traffic_characteristics()
                  .degree_of_multipathing();
        }
      }
      // Default to not bidirectional if bidirectional is not specified in the
      // config.
      bool bidirectional = explicit_pattern.has_bidirectional()
                               ? explicit_pattern.bidirectional()
                               : false;
      auto target_bifurcated_ids =
          SplitHostIdAndBifurcationId(flow.target_host_id());
      CHECK_OK(AddFlow(flow.target_host_id(), target_bifurcated_ids.first,
                       initiator_bifurcated_ids.second,
                       target_bifurcated_ids.second,
                       &traffic_pattern_info_template,
                       /*bidirectional=*/bidirectional))
          << "fail to add a flow.";
    }
  }

  if (traffic_pattern_info_template.is_initiator) {
    for (const auto& config :
         explicit_pattern.traffic_characteristics().conn_config()) {
      ExplicitPatternInfo traffic_pattern_info_per_config =
          traffic_pattern_info_template;
      traffic_pattern_info_per_config.traffic_characteristics
          .connection_config = config;
      SetDefaultValuesInConnectionConfig(
          traffic_pattern_info_per_config.traffic_characteristics
              .connection_config);
      traffic_pattern_info_per_config.traffic_characteristics.offered_load =
          explicit_pattern.traffic_characteristics().offered_load() *
          config.offered_load_ratio();
      traffic_pattern_info_per_config.traffic_characteristics.op_config =
          config.op_config();
      traffic_pattern_info_per_config.avg_interval =
          1e-9 * config.size_distribution_params().mean() * 8 /
          traffic_pattern_info_per_config.traffic_characteristics.offered_load;
      // set the traffic start time
      if (explicit_pattern.traffic_characteristics()
              .has_traffic_start_time_ns()) {
        traffic_pattern_info_per_config.traffic_start_time =
            explicit_pattern.traffic_characteristics().traffic_start_time_ns() /
            1e9;
      }
      traffic_pattern_info_.push_back(traffic_pattern_info_per_config);
    }
  }
}

void TrafficGenerator::InitCompositePattern(
    const TrafficPatternConfig_CompositePattern& composite_pattern) {
  int incast_index = 0;
  int uniform_random_index = 0;
  int explicit_index = 0;

  for (const auto& incast : composite_pattern.incast()) {
    InitIncast(incast, incast_index++);
  }
  for (const auto& uniform_random : composite_pattern.uniform_random()) {
    // Default to 1 repetition if not specified in the UniformRandomTraffic
    // config.
    int repetitions =
        uniform_random.has_repetitions() ? uniform_random.repetitions() : 1;
    for (int i = 0; i < repetitions; i++) {
      InitUniformRandom(uniform_random, uniform_random_index++);
    }
  }
  for (const auto& explicit_pattern : composite_pattern.explicit_pattern()) {
    InitExplicitPattern(explicit_pattern, explicit_index++);
  }
}

absl::Status TrafficGenerator::AddFlow(
    absl::string_view full_remote_host_id, absl::string_view remote_host_id,
    uint8_t source_bifurcation_id, uint8_t destination_bifurcation_id,
    BaseTrafficPatternInfo* const traffic_pattern_info, bool bidirectional) {
  QpOptions qp_options;
  std::unique_ptr<FalconConnectionOptions> connection_options =
      std::make_unique<FalconConnectionOptions>();
  // Gen2 falcon uses the multipath_connection_options to indicate the degree of
  // the multipathing which is required for initializing the connection state
  // accordingly.
  if (falcon_version_ >= 2) {
    auto multipath_connection_options =
        std::make_unique<FalconMultipathConnectionOptions>();
    multipath_connection_options->degree_of_multipathing =
        traffic_pattern_info->traffic_characteristics.degree_of_multipathing;
    connection_options = std::move(multipath_connection_options);
  }
  std::tuple<QpId, QpId> qp;
  switch (traffic_pattern_info->traffic_characteristics.connection_type) {
    case TrafficCharacteristics::UD: {
      LOG(FATAL) << "UD is not proporly implemented with proper interface.";
      break;
    }
    case TrafficCharacteristics::ORDERED_RC: {
      if (bidirectional) {
        ASSIGN_OR_RETURN(
            qp, connection_manager_->GetOrCreateBidirectionalConnection(
                    host_id_, remote_host_id, source_bifurcation_id,
                    destination_bifurcation_id, qp_options, *connection_options,
                    RdmaConnectedMode::kOrderedRc));
      } else {
        ASSIGN_OR_RETURN(
            qp, connection_manager_->CreateUnidirectionalConnection(
                    host_id_, remote_host_id, source_bifurcation_id,
                    destination_bifurcation_id, qp_options, *connection_options,
                    RdmaConnectedMode::kOrderedRc));
      }
      break;
    }
    case TrafficCharacteristics::UNORDERED_RC: {
      if (bidirectional) {
        ASSIGN_OR_RETURN(
            qp, connection_manager_->GetOrCreateBidirectionalConnection(
                    host_id_, remote_host_id, source_bifurcation_id,
                    destination_bifurcation_id, qp_options, *connection_options,
                    RdmaConnectedMode::kUnorderedRc));
      } else {
        ASSIGN_OR_RETURN(
            qp, connection_manager_->CreateUnidirectionalConnection(
                    host_id_, remote_host_id, source_bifurcation_id,
                    destination_bifurcation_id, qp_options, *connection_options,
                    RdmaConnectedMode::kUnorderedRc));
      }
      break;
    }
  }
  auto local_host_id = absl::StrCat(host_id_, "_", source_bifurcation_id);
  qp_to_host_id_[std::make_pair(std::get<0>(qp), std::get<1>(qp))] =
      std::make_pair(local_host_id, full_remote_host_id);
  traffic_pattern_info->create_qps[std::get<0>(qp)].push_back(std::get<1>(qp));
  if (traffic_pattern_info->traffic_pattern_type ==
      TrafficPatternType::kIncast) {
    static_cast<IncastInfo*>(traffic_pattern_info)
        ->incast_remote_host_id_to_qp[full_remote_host_id] =
        std::make_pair(std::get<0>(qp), std::get<1>(qp));
  }
  return absl::OkStatus();
}

double TrafficGenerator::GetInterArrival(
    const BaseTrafficPatternInfo* const traffic_pattern_info) {
  switch (traffic_pattern_info->traffic_characteristics.connection_config
              .arrival_time_distribution()) {
    case TrafficCharacteristics::UNIFORM_TIME_DIST:
      return traffic_pattern_info->avg_interval;
    case TrafficCharacteristics::POISSON_PROCESS:
      return Exponential(rng_.get(), traffic_pattern_info->avg_interval);
  }
}

uint32_t TrafficGenerator::GetMessageSize(
    const BaseTrafficPatternInfo* const traffic_pattern_info) {
  switch (traffic_pattern_info->traffic_characteristics.connection_config
              .msg_size_distribution()) {
    case TrafficCharacteristics::FIXED_SIZE:
      return static_cast<uint32_t>(
          traffic_pattern_info->traffic_characteristics.connection_config
              .size_distribution_params()
              .mean());
    case TrafficCharacteristics::NORMAL_SIZE_DIST:
      return static_cast<uint32_t>(Gaussian(
          rng_.get(),
          traffic_pattern_info->traffic_characteristics.connection_config
              .size_distribution_params()
              .mean(),
          traffic_pattern_info->traffic_characteristics.connection_config
              .size_distribution_params()
              .stddev()));
    case TrafficCharacteristics::UNIFORM_SIZE_DIST:
      return IntUniform(
          rng_.get(),
          traffic_pattern_info->traffic_characteristics.connection_config
              .size_distribution_params()
              .min(),
          traffic_pattern_info->traffic_characteristics.connection_config
              .size_distribution_params()
              .max());
    case TrafficCharacteristics::CUSTOM_SIZE_DIST:
      // Since each quantile has equal probably, uniform-randomly draw a
      // quantile.
      auto quantile = IntUniform(
          rng_.get(), 0,
          traffic_pattern_info->traffic_characteristics.connection_config
              .size_distribution_params()
              .cdf()
              .quantiles()
              .size());
      return traffic_pattern_info->traffic_characteristics.connection_config
          .size_distribution_params()
          .cdf()
          .quantiles()[quantile];
  }
}

RdmaOpcode TrafficGenerator::GetRdmaOp(
    const BaseTrafficPatternInfo* const traffic_pattern_info) {
  TrafficCharacteristics_RdmaOp op =
      traffic_pattern_info->traffic_characteristics.connection_config.op_code();
  if (op == TrafficCharacteristics::RANDOM_OP) {
    if (traffic_pattern_info->traffic_characteristics.connection_type ==
        TrafficCharacteristics::UD) {
      op = static_cast<TrafficCharacteristics_RdmaOp>(
          IntUniform(rng_.get(), 0,
                     static_cast<int>(TrafficCharacteristics::RANDOM_OP) / 2));
    } else {
      double op_ratios[] = {
          traffic_pattern_info->traffic_characteristics.op_config.write_ratio(),
          traffic_pattern_info->traffic_characteristics.op_config.read_ratio(),
          traffic_pattern_info->traffic_characteristics.op_config.send_ratio(),
          traffic_pattern_info->traffic_characteristics.op_config.recv_ratio()};
      TrafficCharacteristics::RdmaOp op_types[] = {
          TrafficCharacteristics::WRITE, TrafficCharacteristics::READ,
          TrafficCharacteristics::SEND, TrafficCharacteristics::RECEIVE};
      double rand_value = DoubleUniform(rng_.get(), 0, 1);
      for (int i = 0; i < 4; i++) {
        if (rand_value < op_ratios[i]) {
          op = op_types[i];
          break;
        }
        rand_value -= op_ratios[i];
      }
      if (op == TrafficCharacteristics::RANDOM_OP) {
        LOG(FATAL) << "Sum of all four types of op ratios < 1";
      }
    }
  }

  switch (op) {
    case TrafficCharacteristics::READ:
      return RdmaOpcode::kRead;
    case TrafficCharacteristics::WRITE:
      return RdmaOpcode::kWrite;
    case TrafficCharacteristics::SEND:
      return RdmaOpcode::kSend;
    case TrafficCharacteristics::RECEIVE:
      return RdmaOpcode::kRecv;
    default:
      LOG(FATAL) << "Wrong op code.";
  }
}

void TrafficGenerator::GenerateOpForUniformRandom(
    UniformRandomInfo* const traffic_pattern) {
  if (traffic_pattern->is_initiator == false) return;

  if (traffic_pattern->traffic_characteristics.connection_config
              .max_generated_packets() != -1 &&
      traffic_pattern->generated_packets >=
          traffic_pattern->traffic_characteristics.connection_config
              .max_generated_packets()) {
    // Reaches the max number of packets that can be generated.
    return;
  }

  // Selects a random qp and generate traffic.
  auto qp =
      std::next(std::begin(traffic_pattern->create_qps),
                IntUniform(rng_.get(), 0, traffic_pattern->create_qps.size()));
  double next_event_time = ScheduleOps(*qp, traffic_pattern);
  CHECK_OK(env_->ScheduleEvent(absl::Seconds(next_event_time),
                               [this, traffic_pattern]() {
                                 GenerateOpForUniformRandom(traffic_pattern);
                               }));
}

void TrafficGenerator::PerformIncastRead(IncastInfo* const traffic_pattern) {
  // If it is READ, the initiator is the victim. We select the victim from
  // incast_initiators, and then select a set of senders from incast_targets.
  // Since the incast_initiators and incast_targets may overlap, it is possible
  // that the selected victim is also selected as the sender. To address this
  // issue, we select N + 1 senders from incast_targets to ensure the final N
  // senders does not include the victim.

  // We must reset the RNG each time to make sure that
  // the random sender/victim selection is the same to all hosts.
  traffic_pattern->incast_selection_rng.seed(
      absl::ToInt64Nanoseconds(env_->ElapsedTime()));
  std::shuffle(traffic_pattern->incast_initiators.begin(),
               traffic_pattern->incast_initiators.end(),
               traffic_pattern->incast_selection_rng);
  std::shuffle(traffic_pattern->incast_targets.begin(),
               traffic_pattern->incast_targets.end(),
               traffic_pattern->incast_selection_rng);

  // Selects the victim (i.e., the initiator).
  const auto& victim = traffic_pattern->incast_initiators.Get(0);
  // For revolving incast, one of the hosts will be choosed as the victim each
  // time with a 1/N probability.
  auto victim_host_id = SplitHostIdAndBifurcationId(victim).first;
  if (victim_host_id != host_id_) {
    return;
  }

  // Selects N out N + 1 sender candidates (i.e., the targets).
  int sender_candidate_num = traffic_pattern->incast_degree + 1;
  sender_candidate_num =
      std::min(sender_candidate_num, traffic_pattern->incast_targets.size());
  int sender_num = traffic_pattern->incast_degree;
  for (int i = 0; i < sender_candidate_num; i++) {
    const auto& selected_sender = traffic_pattern->incast_targets.Get(i);
    if (selected_sender == victim) {
      continue;
    }
    const auto& qp =
        traffic_pattern->incast_remote_host_id_to_qp.at(selected_sender);
    GenerateQpTraffic(qp.first, qp.second, traffic_pattern);
    VLOG(2) << "host: " << host_id_ << " selects sender: " << selected_sender
            << " @time: " << env_->ElapsedTime();

    if (--sender_num == 0) {
      break;
    }
  }
}

void TrafficGenerator::PerformIncastWrite(IncastInfo* const traffic_pattern) {
  // If it is WRITE/SEND, the initiator is the sender. We select the victim from
  // incast_targets, and then select a set of senders from incast_initiators.
  // Since the incast_initiators and incast_targets may overlap, it is possible
  // that the selected victim is also selected as the sender. To address this
  // issue, we select N + 1 senders from incast_initiators to ensure the final N
  // senders does not include the victim.

  // We must reset the RNG each time to
  // make sure that the random sender/victim selection is the same to all hosts.
  traffic_pattern->incast_selection_rng.seed(
      absl::ToInt64Nanoseconds(env_->ElapsedTime()));
  std::shuffle(traffic_pattern->incast_initiators.begin(),
               traffic_pattern->incast_initiators.end(),
               traffic_pattern->incast_selection_rng);
  std::shuffle(traffic_pattern->incast_targets.begin(),
               traffic_pattern->incast_targets.end(),
               traffic_pattern->incast_selection_rng);

  // Selects the victim (i.e., the target).
  const auto& victim = traffic_pattern->incast_targets.Get(0);

  // Selects N out N + 1 sender candidates (i.e., the initiator).
  int sender_candidate_num = traffic_pattern->incast_degree + 1;
  sender_candidate_num =
      std::min(sender_candidate_num, traffic_pattern->incast_initiators.size());
  int sender_num = traffic_pattern->incast_degree;
  for (int i = 0; i < sender_candidate_num; i++) {
    const auto& selected_sender = traffic_pattern->incast_initiators.Get(i);
    if (selected_sender == victim) {
      continue;
    }
    auto select_sender_host_id =
        SplitHostIdAndBifurcationId(selected_sender).first;
    if (select_sender_host_id == host_id_) {
      const auto& qp = traffic_pattern->incast_remote_host_id_to_qp.at(victim);
      GenerateQpTraffic(qp.first, qp.second, traffic_pattern);
      VLOG(2) << "host: " << host_id_
              << " is selected @incast victim: " << victim
              << " @time: " << env_->ElapsedTime();
      break;
    }

    if (--sender_num == 0) {
      break;
    }
  }
}

void TrafficGenerator::GenerateOpForIncast(IncastInfo* const traffic_pattern) {
  if (traffic_pattern->is_initiator == false) return;

  if (traffic_pattern->traffic_characteristics.connection_config
              .max_generated_packets() != -1 &&
      traffic_pattern->generated_packets >=
          traffic_pattern->traffic_characteristics.connection_config
              .max_generated_packets()) {
    // Reaches the max number of packets that can be generated.
    return;
  }

  if (traffic_pattern->traffic_characteristics.connection_config.op_code() ==
      TrafficCharacteristics::READ) {
    PerformIncastRead(traffic_pattern);
  } else {
    PerformIncastWrite(traffic_pattern);
  }

  double next_event_time = GetInterArrival(traffic_pattern);
  CHECK_OK(env_->ScheduleEvent(
      absl::Seconds(next_event_time),
      [this, traffic_pattern]() { GenerateOpForIncast(traffic_pattern); }));
}

void TrafficGenerator::GenerateOpForExplicitPattern(
    ExplicitPatternInfo* const traffic_pattern) {
  if (traffic_pattern->is_initiator == false) return;
  if (traffic_pattern->traffic_characteristics.connection_config
              .max_generated_packets() != -1 &&
      traffic_pattern->generated_packets >=
          traffic_pattern->traffic_characteristics.connection_config
              .max_generated_packets()) {
    // Reaches the max number of packets that can be generated.
    return;
  }
  double next_event_time = 0;
  for (const auto& qp : traffic_pattern->create_qps) {
    // Since the arrival intensity for each Poisson process is the same, we
    // should add them up, which is still a Poisson process with N * arrival
    // intensity. Given that each time N * op size data is issued, so the
    // offered load will not change.
    next_event_time += ScheduleOps(qp, traffic_pattern);
  }
  CHECK_OK(env_->ScheduleEvent(absl::Seconds(next_event_time),
                               [this, traffic_pattern]() {
                                 GenerateOpForExplicitPattern(traffic_pattern);
                               }));
}

void TrafficGenerator::GenerateRdmaOp() {
  // Schedules work for multiple QPs within a single tick.
  for (auto& traffic_pattern : traffic_pattern_info_) {
    if (auto traffic_pattern_ptr =
            std::get_if<ExplicitPatternInfo>(&traffic_pattern)) {
      CHECK_OK(env_->ScheduleEvent(
          absl::Seconds(traffic_pattern_ptr->traffic_start_time +
                        GetInterArrival(traffic_pattern_ptr)),
          [this, traffic_pattern_ptr]() {
            GenerateOpForExplicitPattern(traffic_pattern_ptr);
          }));
    } else if (auto traffic_pattern_ptr =
                   std::get_if<UniformRandomInfo>(&traffic_pattern)) {
      CHECK_OK(env_->ScheduleEvent(
          absl::Seconds(traffic_pattern_ptr->traffic_start_time +
                        GetInterArrival(traffic_pattern_ptr)),
          [this, traffic_pattern_ptr]() {
            GenerateOpForUniformRandom(traffic_pattern_ptr);
          }));
    } else if (auto traffic_pattern_ptr =
                   std::get_if<IncastInfo>(&traffic_pattern)) {
      CHECK_OK(env_->ScheduleEvent(
          absl::Seconds(traffic_pattern_ptr->traffic_start_time +
                        GetInterArrival(traffic_pattern_ptr)),
          [this, traffic_pattern_ptr]() {
            GenerateOpForIncast(traffic_pattern_ptr);
          }));
    }
  }
}

double TrafficGenerator::ScheduleOps(
    const std::pair<QpId, std::vector<QpId>>& qp,
    BaseTrafficPatternInfo* const traffic_pattern_info) {
  // In UD mode, it is possible that the local qp id is mapped to many remote
  // qp ids. Generates one op at a time.
  double delay = 0;
  for (const auto& remote : qp.second) {
    CHECK_OK(env_->ScheduleEvent(
        absl::Seconds(delay), [this, qp, remote, traffic_pattern_info]() {
          GenerateQpTraffic(qp.first, remote, traffic_pattern_info);
        }));
    delay += GetInterArrival(traffic_pattern_info);
  }

  return delay;
}

void TrafficGenerator::GenerateQpTraffic(
    QpId local_qp_id, QpId remote_qp_id,
    BaseTrafficPatternInfo* const traffic_pattern_info) {
  uint32_t msg_size = GetMessageSize(traffic_pattern_info);
  RdmaOpcode op_code = GetRdmaOp(traffic_pattern_info);

  auto host_ids = qp_to_host_id_[{local_qp_id, remote_qp_id}];

  // Collects the registered statistics.
  UpdateOfferedLoadStats(msg_size, op_code, traffic_pattern_info, host_ids);
  UpdateOpSchedulingIntervalStats();
  // Updates the tx bytes per Qp.
  if (stats_collection_ &&
      stats_collection_flags_.enable_per_qp_tx_rx_bytes()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        absl::Substitute(kStatVectorTxBytesPerQp, local_qp_id, remote_qp_id),
        msg_size, StatisticsCollectionConfig::TIME_SERIES_STAT));
  }

  CompletionCallback completion_callback =
      [this, msg_size, op_code, op_issue_time = env_->ElapsedTime(),
       local_qp_id, remote_qp_id,
       traffic_pattern_info](Packet::Syndrome syndrome) {
        if (syndrome == Packet::Syndrome::kAck) {
          UpdateOpLevelStats(traffic_pattern_info, op_code, msg_size,
                             local_qp_id, remote_qp_id, op_issue_time);
        }
      };

  rdma_->PerformOp(local_qp_id, op_code, {msg_size},
                   traffic_pattern_info->traffic_characteristics.inline_message,
                   completion_callback, remote_qp_id);
  traffic_pattern_info->generated_packets++;

  VLOG(2) << host_id_ << " issues op [local_qp_id: " << local_qp_id
          << "; remote_qp_id: " << remote_qp_id << "; msg_size: " << msg_size
          << "; op_code: " << ConvertOpCodeToString(op_code)
          << "; seq_id: " << traffic_pattern_info->generated_packets
          << "] @time: " << env_->ElapsedTime();
}

void TrafficGenerator::UpdateOfferedLoadStats(
    double op_size, RdmaOpcode op_code,
    BaseTrafficPatternInfo* const traffic_pattern_info,
    const std::pair<std::string, std::string> host_ids) {
  if (stats_collection_ == nullptr) {
    return;
  }
  // Collects the total packet size in output vector, and computes the offered
  // load in post-processing.
  // Do not directly compute the offered load here since the sampling interval
  // may not be regular.
  auto stat_name_scalar =
      absl::Substitute(kStatScalarOfferedLoad, host_ids.first, host_ids.second);
  auto stat_name_vector =
      absl::Substitute(kStatVectorOfferedLoad, host_ids.first, host_ids.second);
  auto& total_generated_op_size =
      host_ids_to_stats_[{host_ids.first, host_ids.second}][stat_name_vector];
  total_generated_op_size += op_size * 8 / kGiga;

  if (stats_collection_flags_.enable_vector_offered_load()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        stat_name_vector, total_generated_op_size,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
  }

  if (stats_collection_flags_.enable_scalar_offered_load()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        stat_name_scalar, total_generated_op_size,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
  }

  // The unit is gigabits.
  traffic_pattern_info->generated_op_size_per_pattern += op_size * 8 / kGiga;
  if (stats_collection_flags_.enable_vector_op_stats()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        absl::Substitute(kStatVectorGeneratedOpSize,
                         traffic_pattern_info->traffic_pattern_id,
                         ConvertOpCodeToString(op_code)),
        traffic_pattern_info->generated_op_size_per_pattern,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
  }
}

void TrafficGenerator::UpdateOpSchedulingIntervalStats() {
  double current_time = absl::ToDoubleSeconds(env_->ElapsedTime());
  if (!stats_collection_flags_.enable_op_schedule_interval() ||
      current_time == 0 || stats_collection_ == nullptr) {
    return;
  }
  CHECK_OK(stats_collection_->UpdateStatistic(
      kStatScalarOpScheduleInterval, current_time - last_scheduled_time_,
      StatisticsCollectionConfig::SCALAR_MEAN_STAT));
  last_scheduled_time_ = current_time;
}

void TrafficGenerator::UpdateOpLevelStats(
    BaseTrafficPatternInfo* const traffic_pattern_info, RdmaOpcode op_code,
    uint32_t op_size, QpId local_qp_id, QpId remote_qp_id,
    absl::Duration op_issue_time) {
  if (stats_collection_ == nullptr) return;

  auto host_ids = qp_to_host_id_[{local_qp_id, remote_qp_id}];

  if (op_code == RdmaOpcode::kRead || op_code == RdmaOpcode::kRecv) {
    // Current qp_to_host_id_ map always stores the host pair as (local, remote)
    // regardless of which direction the traffic is flowing.
    std::swap(host_ids.first, host_ids.second);
  }

  double config_op_size;
  if (traffic_pattern_info->traffic_characteristics.connection_config
          .has_size_distribution_params() &&
      traffic_pattern_info->traffic_characteristics.connection_config
          .size_distribution_params()
          .has_mean()) {
    config_op_size =
        traffic_pattern_info->traffic_characteristics.connection_config
            .size_distribution_params()
            .mean();
  } else {
    config_op_size = op_size;
  }

  // Uses the goodput scalar for integration test on forge only.
  auto goodput_stat_name =
      absl::Substitute(kStatScalarGoodput, host_ids.first, host_ids.second);
  auto& total_completed_op_size =
      host_ids_to_stats_[{host_ids.first, host_ids.second}][goodput_stat_name];
  total_completed_op_size += op_size * 8 / kGiga;
  if (stats_collection_flags_.enable_scalar_op_stats()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        goodput_stat_name, total_completed_op_size,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
  }

  // Uses the number of total completed ops (the time series stat) to compute
  // the op rate offline.
  auto completed_ops_stat_name = absl::Substitute(
      kStatScalarCompletedOps, host_ids.first, host_ids.second);
  auto& total_completed_ops = host_ids_to_stats_[{
      host_ids.first, host_ids.second}][completed_ops_stat_name];
  if (stats_collection_flags_.enable_scalar_op_stats()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        completed_ops_stat_name, ++total_completed_ops,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
  }

  if (stats_collection_flags_.enable_vector_op_stats()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        absl::Substitute(kStatVectorCompletedOps, host_ids.first,
                         host_ids.second,
                         traffic_pattern_info->traffic_pattern_id,
                         ConvertOpCodeToString(op_code), config_op_size),
        ++traffic_pattern_info->completed_ops_per_pattern,
        StatisticsCollectionConfig::TIME_SERIES_STAT));

    // Uses the per-op size (the time series stat) to compute the goodput
    // offline.
    CHECK_OK(stats_collection_->UpdateStatistic(
        absl::Substitute(kStatVectorCompletedOpSize, host_ids.first,
                         host_ids.second,
                         traffic_pattern_info->traffic_pattern_id,
                         ConvertOpCodeToString(op_code), config_op_size),
        op_size * 8, StatisticsCollectionConfig::TIME_SERIES_STAT));
  }

  // Updates the op latency.
  double op_latency =
      absl::ToDoubleSeconds(env_->ElapsedTime() - op_issue_time);

  if (stats_collection_flags_.enable_scalar_op_stats()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        absl::Substitute(kStatScalarOpLatency, host_ids.first, host_ids.second,
                         traffic_pattern_info->traffic_pattern_id,
                         ConvertOpCodeToString(op_code), config_op_size),
        op_latency, StatisticsCollectionConfig::SCALAR_MEAN_STAT));
  }

  if (stats_collection_flags_.enable_vector_op_stats()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        absl::Substitute(kStatVectorOpLatency, host_ids.first, host_ids.second,
                         traffic_pattern_info->traffic_pattern_id,
                         ConvertOpCodeToString(op_code), config_op_size),
        op_latency, StatisticsCollectionConfig::TIME_SERIES_STAT));
  }
  // Updates the rx bytes per Qp.
  if (stats_collection_flags_.enable_per_qp_tx_rx_bytes()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        absl::Substitute(kStatVectorRxBytesPerQp, local_qp_id, remote_qp_id),
        op_size, StatisticsCollectionConfig::TIME_SERIES_STAT));
  }
}

}  // namespace isekai
