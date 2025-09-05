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

#ifndef ISEKAI_HOST_RNIC_OMNEST_ENVIRONMENT_H_
#define ISEKAI_HOST_RNIC_OMNEST_ENVIRONMENT_H_

#include <stdint.h>

#include <memory>
#include <random>
#include <utility>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "omnetpp/cmessage.h"
#include "omnetpp/csimplemodule.h"
#include "omnetpp/csimulation.h"
#include "omnetpp/simtime.h"
#include "omnetpp/simtime_t.h"

namespace isekai {

// The time resolution used in the simulation, which should be fixed!
constexpr omnetpp::SimTimeUnit kOmnestTimeResolution = omnetpp::SIMTIME_NS;

// A self message for scheduling events in OMNest via the bound callback
// function.
class CallbackMessage : public omnetpp::cMessage {
 public:
  explicit CallbackMessage(absl::AnyInvocable<void()> callback)
      : omnetpp::cMessage(/* name = */ nullptr, /* kind = */ 0),
        call_back_function_(std::move(callback)) {}

  absl::AnyInvocable<void()>& get_call_back_function() {
    return call_back_function_;
  }

 private:
  absl::AnyInvocable<void()> call_back_function_;
};

// The OMNest environment that translates the scheduled events in the Isekai
// simulation model into OMNest simulation.
class OmnestEnvironment : public Environment {
 public:
  explicit OmnestEnvironment(IsekaiHostInterface* module, absl::string_view key)
      : module_(module) {
    // Seeds rng_ by the module's host_id + host_ip. By doing so, we are sure
    // the results are reproducible.
    std::seed_seq seed(key.begin(), key.end());
    rng_ = std::make_unique<std::mt19937>(seed);
  }

  absl::Status ScheduleEvent(absl::Duration delay,
                             absl::AnyInvocable<void()> callback) override;

  // The unit is nanoseconds (always rounded to kOmnestTimeResolution=1ns).
  absl::Duration ElapsedTime() const override {
    return absl::Nanoseconds(omnetpp::simTime().inUnit(kOmnestTimeResolution));
  }

  // Returns true if provided duration equals elapsed experiment time.
  // Comparison is performed using omnetpp::SimTime to avoid floating point
  // precision difference and account for the simulation time resolution.
  bool ElapsedTimeEquals(absl::Duration duration) const override;

  std::mt19937* GetPrng() const override { return rng_.get(); }
  IsekaiHostInterface* GetModule() const { return module_; }
  absl::Status CallFinish() override;

 private:
  std::unique_ptr<std::mt19937> rng_;
  // The Isekai host bound to the OMNest environment.
  IsekaiHostInterface* const module_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_RNIC_OMNEST_ENVIRONMENT_H_
