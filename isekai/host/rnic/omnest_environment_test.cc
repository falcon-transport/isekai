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

#include <cstdint>
#include <string>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/time/time.h"
#include "glog/logging.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/testing.h"
#include "omnetpp.h"
#include "omnetpp/cmessage.h"
#include "omnetpp/csimplemodule.h"
#include "omnetpp/csimulation.h"
#include "omnetpp/simtime.h"
#include "omnetpp/simtime_t.h"

namespace isekai {
namespace {

using testing::ElementsAre;

// A fake host for testing the omnest environment event scheduling.
class FakeHost : public IsekaiHostInterface {
 public:
  std::string getFullPath() const override { return "fake_host"; }
  // FakeHost handles the event immediately after the event is scheduled.
  // We do not test the actual OMNest scheduling functionality here, instead we
  // test if OMNest environment is workable or not.
  void scheduleAt(omnetpp::simtime_t t, omnetpp::cMessage* msg) override {
    handleMessage(msg);
  }

  void handleMessage(omnetpp::cMessage* msg) override {
    DCHECK(dynamic_cast<CallbackMessage*>(msg) != nullptr) << "wrong msg type.";

    auto* call_back_message = omnetpp::check_and_cast<CallbackMessage*>(msg);
    absl::AnyInvocable<void()>& callback =
        call_back_message->get_call_back_function();
    callback();

    delete msg;
  }

  uint64_t intrand(int64_t r, int rng) const override { return 0; }
};

// Initializes a dummy omnest simulator for testing.
omnetpp::cSimulation* SetupDummyOmnestSimulation() {
  omnetpp::CodeFragments::executeAll(omnetpp::CodeFragments::STARTUP);
  omnetpp::SimTime::setScaleExp(-12);

  omnetpp::cEnvir* env = new omnetpp::cNullEnvir(
      /* ac = */ 0, /* av = */ nullptr, /* c = */ nullptr);
  omnetpp::cSimulation* sim = new omnetpp::cSimulation("simulation", env);
  omnetpp::cSimulation::setActiveSimulation(sim);
  sim->callInitialize();

  return sim;
}

void CloseOmnestSimulation(omnetpp::cSimulation* sim) {
  sim->callFinish();
  omnetpp::cSimulation::setActiveSimulation(nullptr);
  delete sim;
  omnetpp::CodeFragments::executeAll(omnetpp::CodeFragments::SHUTDOWN);
}

TEST(OmnestEnvironmentTest, ScheduleEvent) {
  // Declares the cStaticFlag at the very first beginning to ensure the correct
  // initialization sequence in OMNest.
  omnetpp::cStaticFlag dummy;

  FakeHost host;
  OmnestEnvironment omnest_env(&host, host.getFullPath());
  std::vector<std::string> log;

  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  // The scheduling delay is ignored here (just a placeholder in the unit
  // testing), since FakeHost will execute events immediately.
  EXPECT_OK(omnest_env.ScheduleEvent(absl::Seconds(1),
                                     [&log]() { log.push_back("a"); }));
  EXPECT_OK(omnest_env.ScheduleEvent(absl::Seconds(1),
                                     [&log]() { log.push_back("c"); }));
  EXPECT_OK(omnest_env.ScheduleEvent(absl::Seconds(1), [&log, &omnest_env]() {
    log.push_back("b");
    EXPECT_OK(omnest_env.ScheduleEvent(absl::Seconds(1),
                                       [&log]() { log.push_back("d"); }));
  }));

  EXPECT_THAT(log, ElementsAre("a", "c", "b", "d"));

  CloseOmnestSimulation(dummy_simulator);
}

}  // namespace
}  // namespace isekai
