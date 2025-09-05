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

// Utilities to generate latency with various patterns.
#ifndef ISEKAI_HOST_FALCON_RUE_LATENCY_GENERATOR_H_
#define ISEKAI_HOST_FALCON_RUE_LATENCY_GENERATOR_H_

#include <memory>

#include "absl/memory/memory.h"
#include "absl/random/bit_gen_ref.h"
#include "absl/random/distributions.h"
#include "absl/random/random.h"
#include "absl/time/time.h"

namespace isekai {

// LatencyGeneratorInterface provides flexibility to generate latency with
// different latency models.
class LatencyGeneratorInterface {
 public:
  virtual ~LatencyGeneratorInterface() = default;
  // Generates latency value.
  virtual absl::Duration GenerateLatency() = 0;
};

// A class for fixed processing latency. Thread-safe.
class FixedLatencyGenerator : public LatencyGeneratorInterface {
 public:
  static std::unique_ptr<FixedLatencyGenerator> Create(
      absl::Duration fixed_latency) {
    return std::make_unique<FixedLatencyGenerator>(fixed_latency);
  }

  explicit FixedLatencyGenerator(absl::Duration fixed_latency)
      : fixed_latency_(fixed_latency) {}

  absl::Duration GenerateLatency() override { return fixed_latency_; }

 private:
  absl::Duration fixed_latency_;
};

// A class to generator latency with Gaussian distribution. Thread-safe.
class GaussianLatencyGenerator : public LatencyGeneratorInterface {
 public:
  static std::unique_ptr<GaussianLatencyGenerator> Create(absl::Duration mean,
                                                          absl::Duration stddev,
                                                          absl::Duration min) {
    return std::make_unique<GaussianLatencyGenerator>(mean, stddev, min);
  }

  explicit GaussianLatencyGenerator(absl::Duration mean, absl::Duration stddev,
                                    absl::Duration min)
      : mean_(mean), stddev_(stddev), min_(min), rng_(bit_gen_) {}

  // Generates latency following Gaussian distribution with nanoseconds
  // granularity.
  absl::Duration GenerateLatency() override {
    double duration = absl::Gaussian(rng_, absl::ToDoubleNanoseconds(mean_),
                                     absl::ToDoubleNanoseconds(stddev_));
    duration = std::max(absl::ToDoubleNanoseconds(min_), duration);
    return absl::Nanoseconds(duration);
  }

  // Sets rng_. For mocking in unit tests only.
  void TestSetBitGenRef(absl::BitGenRef rng) { rng_ = rng; }

 private:
  absl::Duration mean_;
  absl::Duration stddev_;
  absl::Duration min_;
  absl::BitGen bit_gen_;
  // Use absl::BitGenRef which allows mocking for unit tests.
  absl::BitGenRef rng_;
};

// A class to generate a periodic latency pattern with period "interval":
// - every "interval_" inject a latency of "burst_".
// - otherwise generate latency of "base_".
// This is class is thread-unsafe.
class BurstLatencyGenerator : public LatencyGeneratorInterface {
 public:
  static std::unique_ptr<BurstLatencyGenerator> Create(uint32_t interval,
                                                       absl::Duration base,
                                                       absl::Duration height) {
    return std::make_unique<BurstLatencyGenerator>(interval, base, height);
  }

  explicit BurstLatencyGenerator(uint32_t interval, absl::Duration base,
                                 absl::Duration burst)
      : interval_(interval), base_(base), burst_(burst), cnt_(0) {}

  absl::Duration GenerateLatency() override {
    cnt_++;
    if (cnt_ == interval_) {
      cnt_ = 0;
      return burst_;
    }
    return base_;
  }

 private:
  uint32_t interval_;
  absl::Duration base_;
  absl::Duration burst_;
  uint32_t cnt_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_RUE_LATENCY_GENERATOR_H_
