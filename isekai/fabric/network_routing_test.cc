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

#include "isekai/fabric/network_routing.h"

#include <cstdint>
#include <vector>

#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/net_address.h"
#include "isekai/common/testing.h"  // IWYU pragma: keep
#include "isekai/common/testing.h"
#include "isekai/fabric/network_router.h"
#include "isekai/fabric/packet_util.h"
#include "isekai/fabric/routing_test_util.h"
#include "omnetpp/cmessage.h"

namespace isekai {

namespace {

class FakeRouter : public NetworkRouter {
 public:
  int GetRouterStage() const override { return 1; }
};

}  // namespace

// This is a peer class that is defined to allow access to private functions
// and variables in NetworkRoutingPipeline for testing purposes.
class NetworkRoutingPipelineTestPeer {
 public:
  void Set(NetworkRoutingPipeline* routing_pipeline) {
    routing_pipeline_ = routing_pipeline;
  }
  absl::StatusOr<uint32_t> GetOutputPort(omnetpp::cMessage* packet) const {
    return routing_pipeline_->GetOutputPort(packet);
  }

 private:
  NetworkRoutingPipeline* routing_pipeline_;
};

// Use the configuration from omnetpp_network_integration test. Host0 connects
// to port0 of router, and host1 connects to port1 of router. The ip addresses
// for host0 and host1 are 2001:db8:85a2::1 and 2001:db8:85a3::1, respectively.
// More network configurations can be found from omnetpp_network_integration.ini
// and test_network_model_integration.ned files.
TEST(NetworkRoutingPipelineTest, GetOutputPortTest) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();
  FakeRouter router;

  NetworkRoutingPipeline routing_pipeline(
      &router, nullptr, "isekai/test_data/single_switch_routing_table.recordio",
      RouterConfigProfile::WCMP);

  NetworkRoutingPipelineTestPeer routing_pipeline_test_peer;
  routing_pipeline_test_peer.Set(&routing_pipeline);
  // Generates a packet sent from host0 to host1.
  ASSERT_OK_AND_ASSIGN(auto mac_addr,
                       MacAddress::OfString("02:00:00:00:00:01"));
  auto host0_to_host1_packet =
      GenerateTestInetPacket("2001:db8:85a2::1", "2001:db8:85a3::1", mac_addr);
  auto selected_port =
      routing_pipeline_test_peer.GetOutputPort(host0_to_host1_packet.get());

  EXPECT_OK(selected_port);
  // The host1 is connected to port1 of the router (see
  // test_network_model_integration.ned), so the selected port should be 1.
  EXPECT_EQ(1, selected_port.value());

  CloseOmnestSimulation(dummy_simulator);
}

// Checks if the router pipeline successfully extract the static port and
// increment the static route index by 1 every time.
TEST(NetworkRoutingPipelineTest, GetOutputPortWithAStaticPortListPacket) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();
  FakeRouter router;

  NetworkRoutingPipeline routing_pipeline(
      &router, nullptr, "isekai/test_data/single_switch_routing_table.recordio",
      RouterConfigProfile::WCMP);

  NetworkRoutingPipelineTestPeer routing_pipeline_test_peer;
  routing_pipeline_test_peer.Set(&routing_pipeline);

  std::vector<uint32_t> expected_static_port_list = {11, 6, 4, 3, 1};

  // Generates a Falcon packet.
  ASSERT_OK_AND_ASSIGN(auto mac_addr,
                       MacAddress::OfString("02:00:00:00:00:01"));
  auto packet =
      GenerateTestInetPacket("2001:db8:85a2::1", "2001:db8:85a3::1", mac_addr);

  // Sets static port list to a Falcon packet.
  SetFalconPacketStaticPortListForTesting(packet.get(),
                                          expected_static_port_list);

  // Every time this packet passes through a router, it increments its static
  // route index. We check if the port selected at each router hop is identical
  // to the corresponding port in the expected static port list.
  for (int i = 0; i < expected_static_port_list.size(); ++i) {
    auto selected_port = routing_pipeline_test_peer.GetOutputPort(packet.get());
    EXPECT_OK(selected_port);
    EXPECT_EQ(expected_static_port_list[i], selected_port.value());
  }
  CloseOmnestSimulation(dummy_simulator);
}

}  // namespace isekai
