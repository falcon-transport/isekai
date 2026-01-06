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

#ifndef ISEKAI_COMMON_DEFAULT_CONFIG_GENERATOR_H_
#define ISEKAI_COMMON_DEFAULT_CONFIG_GENERATOR_H_

#include <cstdint>
#include <limits>

#include "isekai/common/config.pb.h"

namespace isekai {

class DefaultConfigGenerator {
 public:
  // Gen1 Falcon-specific default configuration.
  class Gen1Falcon {
   public:
    static FalconConfig DefaultConfig();

   private:
    static constexpr uint64_t kFalconTickTimeNs = 3;
    // INT32_MAX for all credit types, effectively turning off credit
    // tracking.
    static constexpr uint32_t kMaxCredits = std::numeric_limits<int>::max();
    static FalconConfig::ResourceCredits DefaultResourceCredits();
    // Setting them to 0 effectively means request Xoff will never be
    // asserted.
    static FalconConfig::UlpXoffThresholds DefaultUlpXoffThresholds();
    // Setting them to INT32_MAX effectively means HoL network requests will
    // never be prioritized.
    static FalconConfig::FalconNetworkRequestsOccupancyThresholds
    DefaultFalconNetworkRequestsOccupancyThresholds();
    // Sets the EMA coefficients such that equal weight is given to current
    // occupancy and previous occupancy.
    static FalconConfig::TargetBufferOccupancyEmaCoefficients
    DefaultTargetBufferOccupancyEmaCoefficients();
    static FalconConfig::TargetBufferOccupancyQuantizationTables::
        QuantizationTable
        DefaultQuantizationTable();
    static FalconConfig::ConnectionSchedulerPolicies
    DefaultConnectionSchedulerPolicies();
    static FalconConfig::EarlyRetx DefaultEarlyRetx();
    static FalconConfig::Rue DefaultRue();
  };

  // Gen2 Falcon-specific default configuration.
  class Gen2Falcon {
   public:
    static FalconConfig DefaultConfig();
    static MemoryInterfaceConfig DefaultOnNicDramInterfaceConfig();
    static FalconConfig::PerConnectionBackpressure
    DefaultPerConnectionBackpressure();
  };

  // Gen3 Falcon-specific default configuration.
  class Gen3Falcon {
   public:
    static FalconConfig DefaultConfig();
  };

  // Outputs the default Falcon configuration given the Falcon version number.
  static FalconConfig DefaultFalconConfig(int version);
  static RdmaConfig DefaultRdmaConfig();
  static RNicConfig DefaultRNicConfig();
  static MemoryInterfaceConfig DefaultHostInterfaceConfig();
  // Returns the default config for Traffic Shaper.
  static TrafficShaperConfig DefaultTrafficShaperConfig();

  static StatisticsCollectionConfig::FalconFlags DefaultFalconStatsFlags();
  static StatisticsCollectionConfig::PacketBuilderFlags
  DefaultPacketBuilderStatsFlags();
  static StatisticsCollectionConfig::RdmaFlags DefaultRdmaStatsFlags();
  static StatisticsCollectionConfig::RouterFlags DefaultRouterStatsFlags();
  static StatisticsCollectionConfig::TrafficGeneratorFlags
  DefaultTrafficGeneratorStatsFlags();

 private:
  static inline const uint64_t kDefaultTimingWheelSlots = 1024 * 1024;
};

}  // namespace isekai

#endif  // ISEKAI_COMMON_DEFAULT_CONFIG_GENERATOR_H_
