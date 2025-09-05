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

#include "isekai/host/rnic/omnest_stats_collection.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "glog/logging.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/status_util.h"
#include "omnetpp/checkandcast.h"
#include "omnetpp/chistogram.h"
#include "omnetpp/coutvector.h"
#include "omnetpp/cownedobject.h"
#include "omnetpp/csimplemodule.h"
#include "omnetpp/cstddev.h"

namespace isekai {

StatisticsCollectionConfig OmnestStatisticCollection::stats_collection_config_;

OmnestStatisticCollection::OmnestStatisticCollection(
    std::string_view module_type, omnetpp::cSimpleModule* host_module,
    StatisticsCollectionConfig stats_collection_config)
    : host_module_(host_module) {
  stats_collection_config_ = stats_collection_config;
}

omnetpp::cOwnedObject* OmnestStatisticCollection::RegisterStatisticOutput(
    std::string_view stats_output_name,
    StatisticsCollectionConfig_StatisticsType output_type) {
  if (stats_output_name.empty()) {
    LOG(FATAL) << "Statistic output name cannot be empty.";
  }

  if (stats_outputs_.find(stats_output_name) != stats_outputs_.end()) {
    LOG(FATAL) << "Statistic output name: " << stats_output_name
               << " already registered.";
  }

  std::unique_ptr<omnetpp::cOwnedObject> output_object;
  switch (output_type) {
    case StatisticsCollectionConfig::TIME_SERIES_STAT: {
      output_object = std::make_unique<omnetpp::cOutVector>();
      break;
    }
    case StatisticsCollectionConfig::SCALAR_MAX_STAT:
    case StatisticsCollectionConfig::SCALAR_MIN_STAT:
    case StatisticsCollectionConfig::SCALAR_MEAN_STAT:
    case StatisticsCollectionConfig::SCALAR_SUM_STAT: {
      output_object = std::make_unique<omnetpp::cStdDev>();
      break;
    }
    case StatisticsCollectionConfig::HISTOGRAM_STAT: {
      output_object = std::make_unique<omnetpp::cHistogram>();
      break;
    }
    default:
      LOG(FATAL) << "Unsupported statistic type.";
  }
  output_object->setName(stats_output_name.data());
  stats_outputs_.emplace(stats_output_name,
                         std::make_pair(output_type, std::move(output_object)));
  return stats_outputs_[stats_output_name].second.get();
}

absl::Status OmnestStatisticCollection::UpdateStatistic(
    std::string_view stats_output_name, double value,
    StatisticsCollectionConfig_StatisticsType output_type) {
  auto stats_iter = stats_outputs_.find(stats_output_name);
  omnetpp::cOwnedObject* output_ptr;
  if (stats_iter == stats_outputs_.end()) {
    // If the stat is not registered yet, register it.
    output_ptr = RegisterStatisticOutput(stats_output_name, output_type);
  } else {
    // If the stat is registered already, check if the stat type is consistent.
    if (output_type != stats_iter->second.first) {
      return absl::InternalError("Inconsistent stat output type is used.");
    }
    output_ptr = stats_iter->second.second.get();
  }

  switch (output_type) {
    case StatisticsCollectionConfig::TIME_SERIES_STAT: {
      auto output = omnetpp::check_and_cast<omnetpp::cOutVector*>(output_ptr);
      output->record(value);
      break;
    }
    case StatisticsCollectionConfig::SCALAR_MAX_STAT:
    case StatisticsCollectionConfig::SCALAR_MIN_STAT:
    case StatisticsCollectionConfig::SCALAR_MEAN_STAT:
    case StatisticsCollectionConfig::SCALAR_SUM_STAT: {
      auto output = omnetpp::check_and_cast<omnetpp::cStdDev*>(output_ptr);
      output->collect(value);
      break;
    }
    case StatisticsCollectionConfig::HISTOGRAM_STAT: {
      auto output = omnetpp::check_and_cast<omnetpp::cHistogram*>(output_ptr);
      output->collect(value);
      break;
    }
    default:
      LOG(FATAL) << "unsupported stat type.";
  }
  return absl::OkStatus();
}

void OmnestStatisticCollection::FlushStatistics() {
  for (const auto& output : stats_outputs_) {
    if (output.second.first == StatisticsCollectionConfig::SCALAR_MAX_STAT) {
      auto scalar = omnetpp::check_and_cast<omnetpp::cStdDev*>(
          output.second.second.get());
      host_module_->recordScalar(output.first.c_str(), scalar->getMax());
    } else if (output.second.first ==
               StatisticsCollectionConfig::SCALAR_MIN_STAT) {
      auto scalar = omnetpp::check_and_cast<omnetpp::cStdDev*>(
          output.second.second.get());
      host_module_->recordScalar(output.first.c_str(), scalar->getMin());
    } else if (output.second.first ==
               StatisticsCollectionConfig::SCALAR_MEAN_STAT) {
      auto scalar = omnetpp::check_and_cast<omnetpp::cStdDev*>(
          output.second.second.get());
      host_module_->recordScalar(output.first.c_str(), scalar->getMean());
    } else if (output.second.first ==
               StatisticsCollectionConfig::SCALAR_SUM_STAT) {
      auto scalar = omnetpp::check_and_cast<omnetpp::cStdDev*>(
          output.second.second.get());
      host_module_->recordScalar(output.first.c_str(), scalar->getSum());
    } else if (output.second.first ==
               StatisticsCollectionConfig::HISTOGRAM_STAT) {
      // More details about OMNest histogram can be found here:
      // https://doc.omnetpp.org/omnetpp/api/classomnetpp_1_1cHistogram.html
      auto histogram = omnetpp::check_and_cast<omnetpp::cHistogram*>(
          output.second.second.get());
      histogram->recordAs(output.first.c_str());
    }
  }
}

}  // namespace isekai
