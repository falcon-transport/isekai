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

#ifndef ISEKAI_COMMON_SIMPLE_ENVIRONMENT_H_
#define ISEKAI_COMMON_SIMPLE_ENVIRONMENT_H_

#include <cstdint>
#include <memory>
#include <queue>
#include <random>
#include <utility>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/time/time.h"
#include "isekai/common/environment.h"

namespace isekai {

// A simple simulation environment, intended for tests. It is not thread-safe
// and is expected to run with single thread executing callbacks and scheduling
// events inside it.
class SimpleEnvironment : public Environment {
 public:
  SimpleEnvironment() { rng_ = std::make_unique<std::mt19937>(0); }
  // Runs until there are no more events or Stop() is invoked.
  void Run();
  // Runs until the elapsed time equals to the deadline.
  void RunUntil(absl::Duration deadline);
  // Runs until the elapsed time equals to the given delay plus the current
  // elapsed time.
  void RunFor(absl::Duration delay);

  absl::Status ScheduleEvent(absl::Duration delay,
                             absl::AnyInvocable<void()> callback) override;
  absl::Duration ElapsedTime() const override { return elapsed_time_; }

  bool ElapsedTimeEquals(absl::Duration duration) const override {
    return duration == elapsed_time_;
  }

  std::mt19937* GetPrng() const override { return rng_.get(); }

  absl::Status CallFinish() override {
    Stop();
    return absl::OkStatus();
  }

  // Stops the environment by breaking from the Run() loop.
  void Stop();

  // Returns the number of scheduled events.
  uint64_t ScheduledEvents() const { return scheduled_events_; }
  // Returns the number of executed events.
  uint64_t ExecutedEvents() const { return executed_events_; }

 private:
  struct Event {
    // A unique event id, used for ordering events with the same scheduled_time.
    uint64_t event_id;
    absl::Duration scheduled_time;

    // Mutable so the callback can be std::move()'d after queue_.top()
    mutable absl::AnyInvocable<void()> callback;

    Event(uint64_t event_id, absl::Duration scheduled_time,
          absl::AnyInvocable<void()> callback)
        : event_id(event_id),
          scheduled_time(scheduled_time),
          callback(std::move(callback)) {}

    bool operator<(const Event& other) const {
      // Flip the comparison around so that events with lower timestamps have
      // higher priority.
      if (scheduled_time == other.scheduled_time) {
        return event_id > other.event_id;
      }
      return scheduled_time > other.scheduled_time;
    }
  };

  // Total number of events scheduled, used for assigning event_ids.
  uint64_t scheduled_events_ = 0;
  // Total number of events executed.
  uint64_t executed_events_ = 0;
  // The amount of time passed since the start of the simulation.
  absl::Duration elapsed_time_;
  // Queue of events scheduled in the future, but yet to run.
  std::priority_queue<Event> queue_;
  // Flag to immediately stop the environment from processing further events.
  bool stopped_ = false;
  std::unique_ptr<std::mt19937> rng_;
};

}  // namespace isekai

#endif  // ISEKAI_COMMON_SIMPLE_ENVIRONMENT_H_
