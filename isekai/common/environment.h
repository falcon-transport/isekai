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

#ifndef ISEKAI_COMMON_ENVIRONMENT_H_
#define ISEKAI_COMMON_ENVIRONMENT_H_

#include <random>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/time/time.h"

namespace isekai {

// Abstract interface to the discrete event simulator environment.
class Environment {
 public:
  virtual ~Environment() {}

  // Runs a callback after a simulation-time delay. The given delay must be
  // nonnegative (i.e. events can't be scheduled in the past).
  virtual absl::Status ScheduleEvent(absl::Duration delay,
                                     absl::AnyInvocable<void()> callback) = 0;

  // Returns the amount of time passed since the start of the simulation.
  // absl::Duration has (at least) nanosecond precision, so it can exactly
  // represent something's master timer.
  virtual absl::Duration ElapsedTime() const = 0;

  // Returns true if the simulation elapsed time matches the provided duration,
  // accounting for simulation time resolution. If an event is scheduled at the
  // specified duration, calling this function within the scheduled event will
  // return true. Recommended over directly comparing the ElapsedTime() value,
  // which is truncated based on environment time granularity.
  virtual bool ElapsedTimeEquals(absl::Duration duration) const = 0;

  // Gets a pseudorandom number generator.
  // In order to reproduce simulation results, we use mt19937, which is
  // deterministic and is seeded by a centralized RNG (e.g., provided by
  // OMNest).
  virtual std::mt19937* GetPrng() const = 0;

  // Explicitly calls finish() for all the modules associated with this
  // environment, without waiting for the simulation time limit.
  // This can be used to finish the simulation early when traffic generator is
  // done before the simulation time limit e.g. with fixed-sized traffic.
  virtual absl::Status CallFinish() = 0;
};

}  // namespace isekai

#endif  // ISEKAI_COMMON_ENVIRONMENT_H_
