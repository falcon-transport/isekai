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

#ifndef ISEKAI_HOST_TRAFFIC_TRAFFIC_GENERATOR_H_
#define ISEKAI_HOST_TRAFFIC_TRAFFIC_GENERATOR_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/variant.h"
#include "gtest/gtest_prod.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"

namespace isekai {

enum class TrafficPatternType {
  kIncast,
  kUniformRandom,
  kExplicitPattern,
};

// The TrafficGenerator class drives RDMA traffic.
class TrafficGenerator : public TrafficGeneratorInterface {
 public:
  TrafficGenerator(ConnectionManagerInterface* connection_manager,
                   RdmaBaseInterface* rdma, Environment* env,
                   StatisticCollectionInterface* stats_collector,
                   absl::string_view host_id,
                   const TrafficPatternConfig& traffic_pattern,
                   int falcon_version);
  void GenerateRdmaOp() override;

 private:
  FRIEND_TEST(RdmaAppTest, DISABLED_TestCreateUdQp);
  FRIEND_TEST(RdmaAppTest, TestCreateRcUnorderedQp);
  FRIEND_TEST(RdmaAppTest, TestCreateRcOrderedQp);
  FRIEND_TEST(RdmaAppTestWithParameters, TestGenerateCustomOpSizeDist);

  struct TrafficCharacteristicsInfo {
    TrafficCharacteristics::ConnectionType connection_type;
    TrafficCharacteristics::ConnectionConfig connection_config;
    double offered_load;
    TrafficCharacteristics::OpConfig op_config;
    bool inline_message;
    // Multipathing degree is 1 by default.
    uint32_t degree_of_multipathing = 1;
  };

  // For each traffic pattern, it has the corresponding struct to store the
  // traffic pattern information.
  struct BaseTrafficPatternInfo {
    TrafficPatternType traffic_pattern_type;
    // The traffic characteristics such as injection rate of this traffic
    // pattern.
    TrafficCharacteristicsInfo traffic_characteristics;
    // The average interval = average msg size / offered load.
    double avg_interval;
    // If false, will not initiate traffic generation in this traffic pattern.
    bool is_initiator;
    // The created QPs for this traffic pattern. The key is the local qp id, the
    // value is the corresponding qp. It is possible that one local qp id maps
    // to multiple qps in UD mode.
    // Uses absl::btree_map instead of absl::flat_hash_map in order to keep the
    // created qps in order. This is because in uniform random traffic pattern,
    // we need to let the random qp selection reproducible.
    absl::btree_map<QpId, std::vector<QpId>> create_qps;
    // The total number of generated packets of this traffic pattern.
    uint64_t generated_packets = 0;
    // The id represents the order of this traffic pattern in the composite
    // traffic pattern. For example, if we have two uniform random patterns in
    // the composite traffic pattern, their ids are "uniform_random_0" and
    // "uniform_random_1", respectively.
    std::string traffic_pattern_id;
    double generated_op_size_per_pattern = 0;
    uint64_t completed_ops_per_pattern = 0;
    double traffic_start_time = 0;
  };

  struct IncastInfo : BaseTrafficPatternInfo {
    // -1 means all candidates are selected as senders in the incast.
    uint32_t incast_degree = -1;
    // The set of initiators/targets in this incast.
    ::google::protobuf::RepeatedPtrField<std::string> incast_initiators;
    ::google::protobuf::RepeatedPtrField<std::string> incast_targets;
    // The mapping between remote host and QP for incast.
    absl::flat_hash_map<std::string, std::pair<QpId, QpId>>
        incast_remote_host_id_to_qp;
    // RNG used for sender/victim random selection.
    std::mt19937 incast_selection_rng;

    IncastInfo() : BaseTrafficPatternInfo() {}

    IncastInfo(const IncastInfo& copy)
        : BaseTrafficPatternInfo(copy),
          incast_degree(copy.incast_degree),
          incast_remote_host_id_to_qp(copy.incast_remote_host_id_to_qp) {
      incast_initiators.CopyFrom(copy.incast_initiators);
      incast_targets.CopyFrom(copy.incast_targets);
    }
  };

  struct UniformRandomInfo : BaseTrafficPatternInfo {};

  struct ExplicitPatternInfo : BaseTrafficPatternInfo {};

  // Sets any default values expected for the connection config.
  static void SetDefaultValuesInConnectionConfig(
      TrafficCharacteristics::ConnectionConfig& config);
  // Parses the traffic pattern and initialize the traffic generator.
  void ParseConfig(const TrafficPatternConfig& traffic_pattern);
  // Initializes traffic generator with incast.
  void InitIncast(const TrafficPatternConfig_IncastTraffic& incast_traffic,
                  int index);
  // A wrapper to initialize incast READ and WRITE.
  void InitIncastWrapper(
      const TrafficPatternConfig_IncastTraffic& incast_traffic, int index,
      TrafficCharacteristics::RdmaOp op_code);
  // Initializes traffic generator with uniform random.
  void InitUniformRandom(
      const TrafficPatternConfig_UniformRandomTraffic& uniform_random_traffic,
      int index);
  // Initializes traffic generator with explicit pattern.
  void InitExplicitPattern(
      const TrafficPatternConfig_ExplicitPattern& explicit_pattern, int index);
  // Initializes traffic generator with composite pattern.
  void InitCompositePattern(
      const TrafficPatternConfig_CompositePattern& composite_pattern);
  // Creates QPs based on the traffic pattern.
  absl::Status AddFlow(absl::string_view full_remote_host_id,
                       absl::string_view remote_host_id,
                       uint8_t source_bifurcation_id,
                       uint8_t destination_bifurcation_id,
                       BaseTrafficPatternInfo* traffic_pattern_info,
                       bool bidirectional);
  // Gets the next time stamp to generate op.
  double GetInterArrival(const BaseTrafficPatternInfo* traffic_pattern_info);
  // Gets the op size (in bytes).
  uint32_t GetMessageSize(const BaseTrafficPatternInfo* traffic_pattern_info);
  // Gets the next op code.
  RdmaOpcode GetRdmaOp(const BaseTrafficPatternInfo* traffic_pattern_info);
  // Given a qp, schedules the corresponding RDMA ops.
  // Returns the total elapsed time after scheduling all ops.
  double ScheduleOps(const std::pair<QpId, std::vector<QpId>>& qp,
                     BaseTrafficPatternInfo* traffic_pattern_info);
  // Generates traffic for a single QP.
  void GenerateQpTraffic(QpId local_qp_id, QpId remote_qp_id,
                         BaseTrafficPatternInfo* traffic_pattern_info);
  // Generates the op for different traffic pattern and schedules the next op
  // event.
  void GenerateOpForIncast(IncastInfo* traffic_pattern);
  void GenerateOpForUniformRandom(UniformRandomInfo* traffic_pattern);
  void GenerateOpForExplicitPattern(ExplicitPatternInfo* traffic_pattern);

  void PerformIncastRead(IncastInfo* traffic_pattern);
  void PerformIncastWrite(IncastInfo* traffic_pattern);

  // The offered load will be collected when performing PerformOp().
  void UpdateOfferedLoadStats(double op_size, RdmaOpcode op_code,
                              BaseTrafficPatternInfo* traffic_pattern_info,
                              std::pair<std::string, std::string> host_ids);
  // Updates op-level stats like latency based on the RDMA completion callback.
  void UpdateOpLevelStats(BaseTrafficPatternInfo* traffic_pattern_info,
                          RdmaOpcode op_code, uint32_t op_size,
                          QpId local_qp_id, QpId remote_qp_id,
                          absl::Duration op_issue_time);
  // Updates the RDMA op scheduling interval stats.
  void UpdateOpSchedulingIntervalStats();

  // Getters for traffic_pattern_info_ information, used in unit test only.
  TrafficCharacteristicsInfo get_traffic_characteristics(int index) const {
    return std::visit(
        [](auto&& arg) -> TrafficCharacteristicsInfo {
          return arg.traffic_characteristics;
        },
        traffic_pattern_info_[index]);
  }
  TrafficPatternType get_traffic_pattern_type(int index) const {
    return std::visit(
        [](auto&& arg) -> TrafficPatternType {
          return arg.traffic_pattern_type;
        },
        traffic_pattern_info_[index]);
  }

  // Stores the information like created qps of the traffic pattern. We may have
  // multiple traffic configs on the same connection, so a vector is used to
  // store the information of each traffic config.
  std::vector<absl::variant<ExplicitPatternInfo, UniformRandomInfo, IncastInfo>>
      traffic_pattern_info_;
  // The ID of the host on which the traffic generator is running.
  absl::string_view host_id_;
  // The total amount of generated/completed ops (in bits) in the traffic
  // generator.
  uint64_t total_completed_ops_ = 0;
  // The last scheduled timestamp of generating RDMA op.
  double last_scheduled_time_ = 0;

  absl::flat_hash_map<std::pair<std::string, std::string>,
                      absl::flat_hash_map<std::string, double>>
      host_ids_to_stats_;

  ConnectionManagerInterface* const connection_manager_;
  Environment* const env_;
  RdmaBaseInterface* const rdma_;
  StatisticCollectionInterface* const stats_collection_;
  static StatisticsCollectionConfig::TrafficGeneratorFlags
      stats_collection_flags_;

  // Mapping from (src_qp_id, dst_qp_id) to (local_host_id to remote_id).
  absl::flat_hash_map<std::pair<QpId, QpId>,
                      std::pair<std::string, std::string>>
      qp_to_host_id_;

  int falcon_version_;
  // Random number generator dedicated to this traffic generator class.
  std::unique_ptr<std::mt19937> rng_;
};

};  // namespace isekai

#endif  // ISEKAI_HOST_TRAFFIC_TRAFFIC_GENERATOR_H_
