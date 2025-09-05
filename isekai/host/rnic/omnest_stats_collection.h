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

#ifndef ISEKAI_HOST_RNIC_OMNEST_STATS_COLLECTION_H_
#define ISEKAI_HOST_RNIC_OMNEST_STATS_COLLECTION_H_

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/model_interfaces.h"
#include "omnetpp/cownedobject.h"
#include "omnetpp/csimplemodule.h"

namespace isekai {

// This class works as an intermediate layer between RNic and OMNest simulator
// to help modules in RNic collect various statistics.
class OmnestStatisticCollection : public StatisticCollectionInterface {
 public:
  static constexpr std::string_view kHostStats = "host";
  static constexpr std::string_view kRouterStats = "router";
  // OmnestHost initializes the corresponding OmnestStatisticCollection based on
  // the stats_config. Modules like traffic generator, RDMA model
  // and FALCON model hold pointers to this OmnestStatisticCollection object.
  // module_type maps to kHostStatsCollection/kRouterStatsCollection for
  // host/router.
  OmnestStatisticCollection(std::string_view module_type,
                            omnetpp::cSimpleModule* module,
                            StatisticsCollectionConfig stats_collection_config);
  // Dynamically registers and updates the stats.
  absl::Status UpdateStatistic(
      std::string_view stats_output_name, double value,
      StatisticsCollectionConfig::StatisticsType output_type) override;
  // Returns the stats collection config.
  StatisticsCollectionConfig& GetConfig() { return stats_collection_config_; }
  // The scalar and histogram statistics have to be manually flushed by invoking
  // finish() in OmnestHost.
  // The vector statistics are flushed periodically.
  void FlushStatistics();

 private:
  // Modules in OmnestHost call this function to initialize the specific stats
  // output. For now, the statistic output support time series data (vector),
  // statistic summaries (scalar), and histograms (histogram).
  // NOTE: the statistic output name should be unique in a host.
  // Return the Omnetpp output statistics object.
  omnetpp::cOwnedObject* RegisterStatisticOutput(
      std::string_view stats_output_name,
      StatisticsCollectionConfig::StatisticsType output_type);
  // Stores the registered statistic output. The output could be vector, scalar,
  // or histogram. The stat output type is stored to avoid runtime type check.
  absl::flat_hash_map<std::string,
                      std::pair<StatisticsCollectionConfig::StatisticsType,
                                std::unique_ptr<omnetpp::cOwnedObject>>>
      stats_outputs_;
  omnetpp::cSimpleModule* const host_module_;
  static StatisticsCollectionConfig stats_collection_config_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_RNIC_OMNEST_STATS_COLLECTION_H_
