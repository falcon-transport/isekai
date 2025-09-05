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

#include "isekai/host/rnic/omnest_environment.h"

#include <utility>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/time/time.h"
#include "glog/logging.h"
#include "omnetpp/csimulation.h"
#include "omnetpp/simtime.h"

namespace isekai {

absl::Status OmnestEnvironment::ScheduleEvent(
    absl::Duration delay, absl::AnyInvocable<void()> callback) {
  if (delay < absl::ZeroDuration()) {
    LOG(FATAL) << "Negative delay: " << delay;
  }

  auto* callback_message = new CallbackMessage(std::move(callback));
  if (!callback_message) {
    return absl::InternalError("Fail to allocate callback message.");
  }

  // The scheduled event needs to be wrt a certain host.
  // Converts the delay in seconds for OMNest to schedule events correctly.
  module_->scheduleAt(
      omnetpp::simTime() + omnetpp::SimTime(absl::ToDoubleSeconds(delay)),
      callback_message);

  return absl::OkStatus();
}

bool OmnestEnvironment::ElapsedTimeEquals(absl::Duration duration) const {
  // Convert duration to SimTime, which truncates to kOmnestTimeResolution=1ns.
  auto sim_time_duration = omnetpp::SimTime(absl::ToDoubleSeconds(duration));
  // Take difference between SimTime objects to avoid resolution issues.
  auto sim_time_diff = sim_time_duration - omnetpp::simTime();
  // Convert back to absl::Duration, with kOmnestTimeResolution=1ns.
  return sim_time_diff.inUnit(kOmnestTimeResolution) == 0;
}

//
// CallFinish() is called.
absl::Status OmnestEnvironment::CallFinish() {
  omnetpp::cSimulation* sim = omnetpp::getSimulation();
  if (sim == nullptr) {
    return absl::InternalError("No OMNest simulation found.");
  }
  sim->callFinish();
  return absl::OkStatus();
}

}  // namespace isekai
