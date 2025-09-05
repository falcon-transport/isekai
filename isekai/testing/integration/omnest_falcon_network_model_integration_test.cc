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

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/substitute.h"
#include "glog/logging.h"
#include "omnetpp/simtime_t.h"
#undef NaN

#include "absl/log/check.h"
#include "gtest/gtest.h"
#include "isekai/testing/integration/omnest_embedded_kernel.h"

namespace isekai {
namespace {

constexpr double kSimulationTime = 0.001;  // in seconds.

// Uses a handcrafted routing table to test the functionalities of routing
// pipeline.
TEST(OmnestFalconNetworkModelTest, TestRoutingPipeline) {
  OmnestEmbeddedKernel omnest_sim(
      /* ned_file_dir = */ kTestDir,
      /* ini_file = */ "omnetpp_falcon_network_one_side_op_integration.ini",
      /* network_name = */ "TestFalconNetworkIntegration",
      /* simulation_time = */ kSimulationTime);
  omnest_sim.ExecuteSimulation();

  auto average_goodput_stat_name_0 =
      "traffic_generator.goodput.src_host0_0.dst_host1_0";

  auto average_goodput_stat_name_1 =
      "traffic_generator.goodput.src_host1_0.dst_host0_0";

  double average_goodput_0 = omnest_sim.GetStatistics(absl::StrCat(
                                 "TestFalconNetworkIntegration.host[0].",
                                 average_goodput_stat_name_0)) /
                             kSimulationTime;
  double average_goodput_1 = omnest_sim.GetStatistics(absl::StrCat(
                                 "TestFalconNetworkIntegration.host[1].",
                                 average_goodput_stat_name_1)) /
                             kSimulationTime;
  double pb_packet_discards_0 = omnest_sim.GetStatistics(
      "TestFalconNetworkIntegration.host[0].packet_builder.total_discards");
  double pb_packet_discards_1 = omnest_sim.GetStatistics(
      "TestFalconNetworkIntegration.host[1].packet_builder.total_discards");

  double no_route_discards = omnest_sim.GetStatistics(
      "TestFalconNetworkIntegration.router.router.routing_pipeline.no_route_"
      "discards");
  double ttl_limit_discards = omnest_sim.GetStatistics(
      "TestFalconNetworkIntegration.router.router.routing_pipeline.ttl_limit_"
      "discards");
  double total_discards = omnest_sim.GetStatistics(
      "TestFalconNetworkIntegration.router.router.routing_pipeline.total_"
      "discards");

  double host0_total_tx_packets = omnest_sim.GetStatistics(
      "TestFalconNetworkIntegration.host[0].packet_builder.total_tx_packets");
  double host1_total_tx_packets = omnest_sim.GetStatistics(
      "TestFalconNetworkIntegration.host[1].packet_builder.total_tx_packets");
  double host0_total_rx_packets = omnest_sim.GetStatistics(
      "TestFalconNetworkIntegration.host[0].packet_builder.total_rx_packets");
  double host1_total_rx_packets = omnest_sim.GetStatistics(
      "TestFalconNetworkIntegration.host[1].packet_builder.total_rx_packets");

  // Offered load is 10Gbps, goodput should be around 10Gbps (no packet loss).
  ASSERT_NEAR(average_goodput_0, 10, 0.1);
  ASSERT_NEAR(average_goodput_1, 10, 0.1);
  // No packet should be dropped.
  EXPECT_EQ(pb_packet_discards_0, 0);
  EXPECT_EQ(pb_packet_discards_1, 0);
  EXPECT_EQ(no_route_discards, 0);
  EXPECT_EQ(ttl_limit_discards, 0);
  EXPECT_EQ(total_discards, 0);

  // The router delay = routing pipeline delay + packet enqueue delay (in ns).
  double router_delay = (600 + 10) / 1e9;
  // The offered load is 10Gpbs and the packet size is 1Kb.
  int queueing_packets = router_delay * 1e10 / (1000 * 8);
  // Ideally, host[0].TX == host[1].RX and host[0].RX = host[1].TX. Note that
  // the last generated TX packet may not be transmitted to the network
  // switch. Moreover, there may exist one TX packet on the fly to its
  // receiver, and one TX packet is under receiving.
  EXPECT_NEAR(host0_total_tx_packets, host1_total_rx_packets,
              3 + queueing_packets);
  EXPECT_NEAR(host1_total_tx_packets, host0_total_rx_packets,
              3 + queueing_packets);
}

// The test contains two hosts in the same rack (the routing table is extracted
// from NIB snapshot). When NDv6 flows are included in the routing table, no
// packets should be dropped. On the other hand, if no NDv6 flows are included,
// all packets will be dropped and the testing will fail.
TEST(OmnestFalconNetworkModelTest, TestTwoHostNetworkRouting) {
  OmnestEmbeddedKernel omnest_sim(
      /* ned_file_dir = */ kTestDir,
      /* ini_file = */ "omnetpp_two_falcon_host_network.ini",
      /* network_name = */ "TestFalconNetworkNdv6Flows",
      /* simulation_time = */ kSimulationTime);
  omnest_sim.ExecuteSimulation();

  auto average_goodput_stat_name_0 =
      "traffic_generator.goodput.src_host_101_39_0.dst_host_101_42_0";

  auto average_goodput_stat_name_1 =
      "traffic_generator.goodput.src_host_101_42_0.dst_host_101_39_0";

  double average_goodput_0 =
      omnest_sim.GetStatistics(absl::StrCat(
          "TestFalconNetworkNdv6Flows.host[0].", average_goodput_stat_name_0)) /
      kSimulationTime;
  double average_goodput_1 =
      omnest_sim.GetStatistics(absl::StrCat(
          "TestFalconNetworkNdv6Flows.host[1].", average_goodput_stat_name_1)) /
      kSimulationTime;
  LOG(INFO) << "average_goodput_0: " << average_goodput_0;
  LOG(INFO) << "average_goodput_1: " << average_goodput_1;

  double pb_packet_discards_0 = omnest_sim.GetStatistics(
      "TestFalconNetworkNdv6Flows.host[0].packet_builder.total_discards");
  double pb_packet_discards_1 = omnest_sim.GetStatistics(
      "TestFalconNetworkNdv6Flows.host[1].packet_builder.total_discards");

  double no_route_discards = omnest_sim.GetStatistics(
      "TestFalconNetworkNdv6Flows.router.router.routing_pipeline.no_route_"
      "discards");
  double ttl_limit_discards = omnest_sim.GetStatistics(
      "TestFalconNetworkNdv6Flows.router.router.routing_pipeline.ttl_limit_"
      "discards");
  double total_discards = omnest_sim.GetStatistics(
      "TestFalconNetworkNdv6Flows.router.router.routing_pipeline.total_"
      "discards");
  LOG(INFO) << "no_route_discards: " << no_route_discards;
  LOG(INFO) << "ttl_limit_discards: " << ttl_limit_discards;
  LOG(INFO) << "total_discards: " << total_discards;

  double host0_total_tx_packets = omnest_sim.GetStatistics(
      "TestFalconNetworkNdv6Flows.host[0].packet_builder.total_tx_packets");
  double host1_total_tx_packets = omnest_sim.GetStatistics(
      "TestFalconNetworkNdv6Flows.host[1].packet_builder.total_tx_packets");
  double host0_total_rx_packets = omnest_sim.GetStatistics(
      "TestFalconNetworkNdv6Flows.host[0].packet_builder.total_rx_packets");
  double host1_total_rx_packets = omnest_sim.GetStatistics(
      "TestFalconNetworkNdv6Flows.host[1].packet_builder.total_rx_packets");
  LOG(INFO) << "host0 tx: " << host0_total_tx_packets
            << " ; rx: " << host0_total_rx_packets;
  LOG(INFO) << "host1 tx: " << host1_total_tx_packets
            << " ; rx: " << host1_total_rx_packets;

  // Offered load is 10Gbps, goodput should be around 10Gbps (no packet loss).
  ASSERT_NEAR(average_goodput_0, 10, 0.1);
  ASSERT_NEAR(average_goodput_1, 10, 0.1);
  // No packet should be dropped.
  EXPECT_EQ(pb_packet_discards_0, 0);
  EXPECT_EQ(pb_packet_discards_1, 0);
  EXPECT_EQ(no_route_discards, 0);
  EXPECT_EQ(ttl_limit_discards, 0);
  EXPECT_EQ(total_discards, 0);

  // The router delay = routing pipeline delay + packet enqueue delay (in ns).
  double router_delay = (600 + 10) / 1e9;
  // The offered load is 10Gpbs and the packet size is 1Kb.
  int queueing_packets = router_delay * 1e10 / (1000 * 8);
  // host[0].TX == host[1].RX and host[0].RX = host[1].TX. Note that the last
  // generated TX packet may not be transmitted to the network switch. Moreover,
  // there may exist one TX packet on the fly to its receiver, and one
  // TX packet is under receiving.
  EXPECT_NEAR(host0_total_tx_packets, host1_total_rx_packets,
              3 + queueing_packets);
  EXPECT_NEAR(host1_total_tx_packets, host0_total_rx_packets,
              3 + queueing_packets);
}

// Two hosts are connected via a complete network model. Uses explicit traffic
// pattern: host0 sends 10 packets to host1.
TEST(OmnestFalconNetworkModelTest, TestTwoHostInterRack) {
  OmnestEmbeddedKernel omnest_sim(
      /* ned_file_dir = */ kTestDir,
      /* ini_file = */ "omnetpp_falcon_inter_rack_simulation.ini",
      /* network_name = */ "TestFalconNetworkInterRack",
      /* simulation_time = */ kSimulationTime);
  omnest_sim.ExecuteSimulation();

  double host0_total_tx_packets = omnest_sim.GetStatistics(
      "TestFalconNetworkInterRack.host0.packet_builder.total_tx_packets");
  double host1_total_rx_packets = omnest_sim.GetStatistics(
      "TestFalconNetworkInterRack.host1.packet_builder.total_rx_packets");
  LOG(INFO) << "host0 tx: " << host0_total_tx_packets
            << " host1 rx: " << host1_total_rx_packets;

  // The router delay = routing pipeline delay + packet enqueue delay (in ns).
  double router_delay = (600 + 10) / 1e9;
  // The offered load is 10Gpbs and the packet size is 1Kb.
  int queueing_packets = router_delay * 1e10 / (1000 * 8);
  // host[0].TX == host[1].RX. Note that the last
  // generated TX packet may not be transmitted to the network switch. Moreover,
  // there may exist one TX packet on the fly to its receiver, and one is under
  // receiving.
  EXPECT_NEAR(host0_total_tx_packets, host1_total_rx_packets,
              3 + queueing_packets);

  // Checks if there is any packet drop in the network.
  for (int i = 0; i < 124; i++) {
    double no_route_discards = omnest_sim.GetStatistics(
        absl::Substitute("TestFalconNetworkInterRack.routers[$0].router."
                         "routing_pipeline.no_route_discards",
                         i));
    double ttl_limit_discards = omnest_sim.GetStatistics(
        absl::Substitute("TestFalconNetworkInterRack.routers[$0].router."
                         "routing_pipeline.ttl_limit_discards",
                         i));
    double total_discards = omnest_sim.GetStatistics(absl::Substitute(
        "TestFalconNetworkInterRack.routers[$0].router.routing_pipeline.total_"
        "discards",
        i));
    EXPECT_EQ(no_route_discards, 0);
    EXPECT_EQ(ttl_limit_discards, 0);
    EXPECT_EQ(total_discards, 0);
  }
}

// Creates a test scenario where two hosts send packets to each other. The
// connection between two hosts is bidirectional. Offered load is 10Gbps, op
// size is 128KB, op type is READ, and op arrival time follows Poisson
// distribution.
TEST(OmnestFalconNetworkModelTest, TestBidirectionConnection) {
  OmnestEmbeddedKernel omnest_sim(
      /* ned_file_dir = */ kTestDir,
      /* ini_file = */ "omnetpp_falcon_network_integration_bidirection.ini",
      /* network_name = */ "TestFalconNetworkIntegration",
      /* simulation_time = */ kSimulationTime);
  omnest_sim.ExecuteSimulation();

  double host0_total_tx_packets = omnest_sim.GetStatistics(
      "TestFalconNetworkIntegration.host[0].packet_builder.total_tx_packets");
  double host1_total_tx_packets = omnest_sim.GetStatistics(
      "TestFalconNetworkIntegration.host[1].packet_builder.total_tx_packets");
  double host0_total_rx_packets = omnest_sim.GetStatistics(
      "TestFalconNetworkIntegration.host[0].packet_builder.total_rx_packets");
  double host1_total_rx_packets = omnest_sim.GetStatistics(
      "TestFalconNetworkIntegration.host[1].packet_builder.total_rx_packets");
  LOG(INFO) << "host0 tx: " << host0_total_tx_packets
            << " ; rx: " << host0_total_rx_packets;
  LOG(INFO) << "host1 tx: " << host1_total_tx_packets
            << " ; rx: " << host1_total_rx_packets;
  EXPECT_GT(host0_total_tx_packets, 10 * 1e9 * kSimulationTime / (4000 * 8));
  EXPECT_GT(host1_total_tx_packets, 10 * 1e9 * kSimulationTime / (4000 * 8));
}

// Check if traffic follows all the configured ports of the specified routers in
// the static port forward and reverse lists, in Gen2.
TEST(OmnestFalconNetworkModelTest, TestGen2InterRackStaticRouting) {
  OmnestEmbeddedKernel omnest_sim(
      /* ned_file_dir = */ kTestDir,
      /* ini_file = */
      "omnetpp_falcon_gen2_inter_rack_static_route_simulation.ini",
      /* network_name = */ "TestFalconNetworkInterRack",
      /* simulation_time = */ kSimulationTime);
  omnest_sim.ExecuteSimulation();

  // The configurations below are copied from the configuration file
  // 'falcon_gen2_inter_rack_static_route_config.pb.txt'. The router and port
  // information and connectivity, are obtained from the corresponding
  // NED file.

  // A total of 4 paths (4 static route port lists) are used for this test.
  // The paths will use the corresponding static route port list with the flow
  // id as an index in Gen2.

  // path_static_routing_port_list {
  //   # Host 16 ->
  //   # Router 116 port 5  ->
  //   # Router 16  port 0  ->
  //   # Router 17  port 1  ->
  //   # Router 22  port 16 ->
  //   # Router 66  port 38 ->
  //   # Host 38
  //   forward_routing_port_list: "[5, 0, 1, 16, 38]"
  //   # Host 38 ->
  //   # Router 66  port 3  ->
  //   # Router 22  port 11 ->
  //   # Router 17  port 7  ->
  //   # Router 16  port 26 ->
  //   # Router 116 port 16 ->
  //   # Host 16
  //   reverse_routing_port_list: "[3, 11, 7, 26, 16]"
  // }
  // path_static_routing_port_list {
  //   # Host 16 ->
  //   # Router 116 port 3  ->
  //   # Router 18  port 0  ->
  //   # Router 19  port 1  ->
  //   # Router 20  port 16 ->
  //   # Router 66  port 38 ->
  //   # Host 38
  //   forward_routing_port_list: "[3, 0, 1, 16, 38]"
  //   # Host 38 ->
  //   # Router 66  port 5  ->
  //   # Router 20  port 11 ->
  //   # Router 19  port 7  ->
  //   # Router 18  port 26 ->
  //   # Router 116 port 16 ->
  //   # Host 16
  //   reverse_routing_port_list: "[5, 11, 7, 26, 16]"
  // }
  // path_static_routing_port_list {
  //   # Host 16 ->
  //   # Router 116 port 5  ->
  //   # Router 16  port 0  ->
  //   # Router 17  port 1  ->
  //   # Router 22  port 16 ->
  //   # Router 66  port 38 ->
  //   # Host 38
  //   forward_routing_port_list: "[5, 0, 1, 16, 38]"
  //   # Host 38 ->
  //   # Router 66  port 3  ->
  //   # Router 22  port 11 ->
  //   # Router 17  port 7  ->
  //   # Router 16  port 26 ->
  //   # Router 116 port 16 ->
  //   # Host 16
  //   reverse_routing_port_list: "[3, 11, 7, 26, 16]"
  // }
  // path_static_routing_port_list {
  //   # Host 16 ->
  //   # Router 116 port 3  ->
  //   # Router 18  port 0  ->
  //   # Router 19  port 1  ->
  //   # Router 20  port 16 ->
  //   # Router 66  port 38 ->
  //   # Host 38
  //   forward_routing_port_list: "[3, 0, 1, 16, 38]"
  //   # Host 38 ->
  //   # Router 66  port 5  ->
  //   # Router 20  port 11 ->
  //   # Router 19  port 7  ->
  //   # Router 18  port 26 ->
  //   # Router 116 port 16 ->
  //   # Host 16
  //   reverse_routing_port_list: "[5, 11, 7, 26, 16]"
  // }

  // The expected lists of routers and their ports used for static routes, these
  // are identical to the configuration above.
  // Path1 and Path3 use the same port list and follow the same forward and
  // reverse paths. Path2 and Path4 also share the same port list and follow the
  // same forward and reverse paths. Therefore, we only need to check the port
  // and router lists of the static routing for Path1 and Path3.
  std::vector<std::vector<uint32_t>> expected_forward_routing_router_list = {
      {116, 16, 17, 22, 66}, {116, 18, 19, 20, 66}};
  std::vector<std::vector<uint32_t>> expected_forward_routing_port_list = {
      {5, 0, 1, 16, 38}, {3, 0, 1, 16, 38}};

  std::vector<std::vector<uint32_t>> expected_reverse_routing_router_list = {
      {66, 22, 17, 16, 116}, {66, 20, 19, 18, 116}};
  std::vector<std::vector<uint32_t>> expected_reverse_routing_port_list = {
      {3, 11, 7, 26, 16}, {5, 11, 7, 26, 16}};

  // Total number of paths and total number of ports in the lists.
  uint32_t num_paths = expected_forward_routing_router_list.size();
  uint32_t num_ports = expected_forward_routing_router_list[0].size();

  // Checks if traffic has passed through all the configured ports of the
  // specified routers in the static port forward and reverse lists.
  for (int path_idx = 0; path_idx < num_paths; path_idx++) {
    for (int port_idx = 0; port_idx < num_ports; port_idx++) {
      double forward_tx_packets = omnest_sim.GetStatistics(absl::Substitute(
          "TestFalconNetworkInterRack.routers[$0].router."
          "port$1.tx_packets",
          expected_forward_routing_router_list[path_idx][port_idx],
          expected_forward_routing_port_list[path_idx][port_idx]));
      CHECK_GT(forward_tx_packets, 0);
      double reverse_tx_packets = omnest_sim.GetStatistics(absl::Substitute(
          "TestFalconNetworkInterRack.routers[$0].router."
          "port$1.tx_packets",
          expected_reverse_routing_router_list[path_idx][port_idx],
          expected_reverse_routing_port_list[path_idx][port_idx]));
      CHECK_GT(reverse_tx_packets, 0);
    }
  }
}

// Sanity check 2nd generation Falcon modules.
//
// integration tests.
TEST(OmnestFalconNetworkModelTest, TestGen2BidirectionConnection) {
  double gen2_host0_total_tx_packets;
  double gen2_host1_total_tx_packets;
  double gen2_host0_total_rx_packets;
  double gen2_host1_total_rx_packets;

  OmnestEmbeddedKernel omnest_gen2_sim(
      /* ned_file_dir = */ kTestDir,
      /* ini_file = */
      "omnetpp_falcon_gen2_network_integration_bidirection.ini",
      /* network_name = */ "TestFalconNetworkIntegration",
      /* simulation_time = */ kSimulationTime);
  omnest_gen2_sim.ExecuteSimulation();

  gen2_host0_total_tx_packets = omnest_gen2_sim.GetStatistics(
      "TestFalconNetworkIntegration.host[0].packet_builder.total_tx_packets");
  gen2_host1_total_tx_packets = omnest_gen2_sim.GetStatistics(
      "TestFalconNetworkIntegration.host[1].packet_builder.total_tx_packets");

  gen2_host0_total_rx_packets = omnest_gen2_sim.GetStatistics(
      "TestFalconNetworkIntegration.host[0].packet_builder.total_rx_packets");
  gen2_host1_total_rx_packets = omnest_gen2_sim.GetStatistics(
      "TestFalconNetworkIntegration.host[1].packet_builder.total_rx_packets");

  EXPECT_GT(gen2_host0_total_tx_packets,
            10 * 1e9 * kSimulationTime / (4000 * 8));
  EXPECT_GT(gen2_host1_total_tx_packets,
            10 * 1e9 * kSimulationTime / (4000 * 8));
  EXPECT_EQ(gen2_host0_total_rx_packets, gen2_host1_total_tx_packets);
  EXPECT_EQ(gen2_host1_total_rx_packets, gen2_host0_total_tx_packets);
}

TEST(OmnestFalconNetworkModelTest, DISABLED_TestSendAndRecvOps) {
  OmnestEmbeddedKernel omnest_sim(
      /* ned_file_dir = */ kTestDir,
      /* ini_file = */ "omnetpp_falcon_network_two_side_op_integration.ini",
      /* network_name = */ "TestFalconNetworkIntegration",
      /* simulation_time = */ kSimulationTime);
  omnest_sim.ExecuteSimulation();

  double average_send_goodput =
      omnest_sim.GetStatistics(
          "TestFalconNetworkIntegration.host[0].traffic_generator.goodput.src_"
          "host0_0.dst_host1_0") /
      kSimulationTime;
  double average_recv_goodput =
      omnest_sim.GetStatistics(
          "TestFalconNetworkIntegration.host[1].traffic_generator.goodput.src_"
          "host1_0.dst_host0_0") /
      kSimulationTime;

  // Offered load is 10Gbps, goodput for send and recv should be around 10Gbps
  // (no packet loss).
  ASSERT_NEAR(average_send_goodput, 10, 0.1);
  ASSERT_NEAR(average_recv_goodput, 10, 0.1);
}

// Integration test for solicitation write with bidirectional connections.
// 1-to-1 communication with 100 ops issued.
TEST(OmnestFalconNetworkModelTest,
     TestSolicitationWriteWithBidirectionalConnection) {
  OmnestEmbeddedKernel omnest_sim(
      /* ned_file_dir = */ kTestDir,
      /* ini_file = */
      "omnetpp_falcon_network_solicitation_write_bidirectional.ini",
      /* network_name = */ "TestFalconNetworkIntegration",
      /* simulation_time = */ kSimulationTime);
  omnest_sim.ExecuteSimulation();

  auto average_goodput_stat_name_0 =
      "traffic_generator.goodput.src_host0_0.dst_host1_0";

  auto average_goodput_stat_name_1 =
      "traffic_generator.goodput.src_host1_0.dst_host0_0";

  // Total amount of completed op size in bits.
  double total_completed_op_data_1 =
      omnest_sim.GetStatistics(
          absl::StrCat("TestFalconNetworkIntegration.host[0].",
                       average_goodput_stat_name_0)) *
      1e9;
  double total_completed_op_data_2 =
      omnest_sim.GetStatistics(
          absl::StrCat("TestFalconNetworkIntegration.host[1].",
                       average_goodput_stat_name_1)) *
      1e9;

  // Each op size is 121600B, in total 100 ops. So the total transmitted data is
  // 121600 * 8 * 100.
  double expected_completed_op_data = 121600 * 8 * 100;
  EXPECT_DOUBLE_EQ(total_completed_op_data_1, expected_completed_op_data);
  EXPECT_DOUBLE_EQ(total_completed_op_data_2, expected_completed_op_data);
}

TEST(OmnestFalconNetworkModelTest, TestingBifurrcatedHost) {
  OmnestEmbeddedKernel omnest_sim(
      /* ned_file_dir = */ kTestDir,
      /* ini_file = */
      "omnetpp_bifurcated_hosts.ini",
      /* network_name = */ "TestFalconNetworkIntegration",
      /* simulation_time = */ kSimulationTime);
  omnest_sim.ExecuteSimulation();

  auto average_goodput_stat_name_0 =
      "traffic_generator.goodput.src_host1_3.dst_host0_1";

  // Total amount of completed op size in bits.
  double total_completed_op_data_1 =
      omnest_sim.GetStatistics(
          absl::StrCat("TestFalconNetworkIntegration.host[1].",
                       average_goodput_stat_name_0)) *
      1e9;
  // Each op size is 131072 bytes, in total 100 ops. So the total transmitted
  // data should be 131072 * 8 * 100.
  double expected_completed_op_data = 65536 * 8 * 100;
  EXPECT_EQ(std::ceil(total_completed_op_data_1), expected_completed_op_data);
}

}  // namespace
}  // namespace isekai
