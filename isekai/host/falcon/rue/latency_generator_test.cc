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

#include "isekai/host/falcon/rue/latency_generator.h"

#include <cstdint>

#include "absl/random/mock_distributions.h"
#include "absl/random/mocking_bit_gen.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace isekai {

namespace {
constexpr int32_t kRepetitions = 5000;

// Test FixedLatencyGenerator.
TEST(FixedLatencyGeneratorTest, Success) {
  const absl::Duration fixed_latency_ns = absl::Nanoseconds(100);
  std::unique_ptr<LatencyGeneratorInterface> latency_gen =
      FixedLatencyGenerator::Create(fixed_latency_ns);
  for (int i = 0; i < kRepetitions; i++) {
    EXPECT_EQ(latency_gen->GenerateLatency(), fixed_latency_ns);
  }
}

// Test BurstLatencyGenerator.
TEST(BurstLatencyGeneratorTest, Success) {
  const uint32_t interval = 49;
  const absl::Duration base_ns = absl::Nanoseconds(100);
  const absl::Duration burst_ns = absl::Nanoseconds(1000);

  std::unique_ptr<LatencyGeneratorInterface> latency_gen =
      BurstLatencyGenerator::Create(interval, base_ns, burst_ns);

  for (int i = 0; i < kRepetitions; i++) {
    if ((i + 1) % interval != 0) {
      EXPECT_EQ(latency_gen->GenerateLatency(), base_ns);
    } else {
      EXPECT_EQ(latency_gen->GenerateLatency(), burst_ns);
    }
  }
}

// Test GaussianLatencyGenerator.
TEST(GaussianLatencyGeneratorTest, Success) {
  const absl::Duration mean_ns = absl::Nanoseconds(200);
  const absl::Duration stddev_ns = absl::Nanoseconds(20);
  const absl::Duration min_ns = absl::Nanoseconds(10);

  std::unique_ptr<GaussianLatencyGenerator> latency_gen =
      GaussianLatencyGenerator::Create(mean_ns, stddev_ns, min_ns);

  absl::MockingBitGen gen;
  latency_gen->TestSetBitGenRef(gen);
  EXPECT_CALL(absl::MockGaussian<double>(),
              Call(gen, absl::ToDoubleNanoseconds(mean_ns),
                   absl::ToDoubleNanoseconds(stddev_ns)))
      .WillOnce(testing::Return(absl::ToDoubleNanoseconds(mean_ns)));

  EXPECT_EQ(latency_gen->GenerateLatency(), mean_ns);
}

TEST(GaussianLatencyGeneratorTest, SuccessWithLowerBound) {
  const absl::Duration mean_ns = absl::Nanoseconds(20);
  const absl::Duration stddev_ns = absl::Nanoseconds(20);
  const absl::Duration min_ns = absl::Nanoseconds(15);
  // Make the random generator returns a value smaller than min_ns.
  const double gaussian_return_val = 10;

  std::unique_ptr<GaussianLatencyGenerator> latency_gen =
      GaussianLatencyGenerator::Create(mean_ns, stddev_ns, min_ns);

  absl::MockingBitGen gen;
  latency_gen->TestSetBitGenRef(gen);
  EXPECT_CALL(absl::MockGaussian<double>(),
              Call(gen, absl::ToDoubleNanoseconds(mean_ns),
                   absl::ToDoubleNanoseconds(stddev_ns)))
      .WillOnce(testing::Return(gaussian_return_val));

  EXPECT_EQ(latency_gen->GenerateLatency(), min_ns);
}

}  // namespace

}  // namespace isekai
