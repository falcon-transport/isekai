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

#include "isekai/fabric/arbiter.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/fabric/routing_test_util.h"
#include "omnetpp/cmessage.h"

namespace isekai {

namespace {

TEST(ArbiterTest, PriorityScheduling) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  auto arbiter = CreateArbiter(RouterConfigProfile::FIXED_PRIORITY);
  std::vector<const omnetpp::cMessage*> packet_list(3, nullptr);
  EXPECT_EQ(-1, arbiter->Arbitrate(packet_list));

  omnetpp::cMessage packet;
  packet_list[0] = &packet;
  EXPECT_EQ(0, arbiter->Arbitrate(packet_list));

  packet_list[0] = nullptr;
  packet_list[1] = &packet;
  packet_list[2] = &packet;
  EXPECT_EQ(1, arbiter->Arbitrate(packet_list));

  CloseOmnestSimulation(dummy_simulator);
}

}  // namespace

}  // namespace isekai
