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

#include "isekai/common/simple_environment.h"

#include <queue>
#include <utility>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/time/time.h"
#include "glog/logging.h"

namespace isekai {

void SimpleEnvironment::Run() {
  while (!stopped_ && !queue_.empty()) {
    const Event& next_event = queue_.top();
    elapsed_time_ = next_event.scheduled_time;
    absl::AnyInvocable<void()> callback = std::move(next_event.callback);
    queue_.pop();

    callback();
    ++executed_events_;
  }
}

// The simulation environement will be paused by calling RunUntil(), and it can
// be resumed by calling RunUntil() again.
void SimpleEnvironment::RunUntil(absl::Duration deadline) {
  CHECK(deadline > elapsed_time_)
      << "Deadline " << deadline
      << " is smaller than the current simulation time";
  // Sets the event id to -1, so that RunUntil() will first execute all the
  // scheduled events having the same timestamp as the breaking point, and then
  // pause the envrionment.
  queue_.emplace(-1, deadline, [this]() { Stop(); });
  stopped_ = false;

  Run();
}

void SimpleEnvironment::RunFor(absl::Duration delay) {
  CHECK(delay >= absl::ZeroDuration()) << "Delay " << delay << " is negative";
  queue_.emplace(-1, elapsed_time_ + delay, [this]() { Stop(); });
  stopped_ = false;

  Run();
}

void SimpleEnvironment::Stop() { stopped_ = true; }

absl::Status SimpleEnvironment::ScheduleEvent(
    absl::Duration delay, absl::AnyInvocable<void()> callback) {
  if (delay < absl::ZeroDuration()) {
    LOG(FATAL) << "Negavie delay: " << delay;
  }
  queue_.emplace(++scheduled_events_, elapsed_time_ + delay,
                 std::move(callback));

  return absl::OkStatus();
}

}  // namespace isekai
