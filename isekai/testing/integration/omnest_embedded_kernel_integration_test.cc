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

#include "absl/strings/str_cat.h"
#include "absl/strings/substitute.h"
#include "glog/logging.h"
#include "isekai/common/model_interfaces.h"

#undef NaN
#include "gtest/gtest.h"
#include "isekai/testing/integration/omnest_embedded_kernel.h"

// Some bases of the simulation setup in the unit testing:
// 1. Two hosts, each of which generates packets at rate 10Gbps, and the packet
// size is fixed at 1Kb.
// 2. Two hosts are connected directly via a channel with data rate = 100Gbps
// and delay = 1us.

namespace isekai {
namespace {

constexpr double kSimulationTime = 0.001;  // in seconds.

// Correct offered load indicates that overall the TestHost works OK.
TEST(OmnestEnvironmentTest, TestOfferedLoad) {
  OmnestEmbeddedKernel omnest_sim(/* ned_file_dir = */ kTestDir,
                                  /* ini_file = */ "omnetpp.ini",
                                  /* network_name = */ "TestFalconNetwork",
                                  /* simulation_time = */ kSimulationTime);
  omnest_sim.ExecuteSimulation();

  auto stat_name_scalar =
      "traffic_generator.offered_load_scalar.src_host0_0.dst_host1_0";

  double average_offered_load =
      omnest_sim.GetStatistics(
          absl::StrCat("TestFalconNetwork.host[0].", stat_name_scalar)) /
      kSimulationTime;

  // The difference should be within 10Mb/s.
  ASSERT_NEAR(average_offered_load, 10, 0.01);
}

// Correct rdma op scheduling interval indicates that the Isekai events are
// scheduled correctly.
TEST(OmnestEnvironmentTest, TestEventScheduling) {
  OmnestEmbeddedKernel omnest_sim(/* ned_file_dir = */ kTestDir,
                                  /* ini_file = */ "omnetpp.ini",
                                  /* network_name = */ "TestFalconNetwork",
                                  /* simulation_time = */ kSimulationTime);
  omnest_sim.ExecuteSimulation();

  double average_interval = omnest_sim.GetStatistics(
      "TestFalconNetwork.host[0].traffic_generator.op_schedule_interval");

  // The offered load is 10Gb/s, while the op size is 100 bytes (800 bits). So
  // the expected average internal is 800 / (10 * 1e9).
  ASSERT_DOUBLE_EQ(average_interval, 100 * 8 / (10 * kGiga));
}

// Correct end-to-end packet delay indicates that the simulation network works
// as expected.
TEST(OmnestEnvironmentTest, TestPacketTransmission) {
  OmnestEmbeddedKernel omnest_sim(/* ned_file_dir = */ kTestDir,
                                  /* ini_file = */ "omnetpp_latency_eval.ini",
                                  /* network_name = */ "TestFalconNetwork",
                                  /* simulation_time = */ kSimulationTime);
  omnest_sim.ExecuteSimulation();

  // By default, the T1 timestamp is taken after PB, so PB queueing delay is not
  // included.
  double average_delay = omnest_sim.GetStatistics(
      "TestFalconNetwork.host[0].packet_builder.scalar_rx_packet_delay");

  // The op size is 100 bytes, while the link speed is 100Gbps.
  double transmission_delay = 100 * 8 / (100 * kGiga);
  // The delay propagation delay is 10/2e8 (see
  // https://doc.omnetpp.org/inet/api-current/neddoc/inet.node.ethernet.EtherLink.html)
  double propagation_delay = 5e-8;
  ASSERT_NEAR(average_delay, transmission_delay + propagation_delay,
              transmission_delay + propagation_delay);
}

TEST(OmnestEnvironmentTest, TestPacketBuilderPacketRate) {
  OmnestEmbeddedKernel omnest_sim(/* ned_file_dir = */ kTestDir,
                                  /* ini_file = */ "omnetpp.ini",
                                  /* network_name = */ "TestFalconNetwork",
                                  /* simulation_time = */ kSimulationTime);
  omnest_sim.ExecuteSimulation();

  double packet_send_rate =
      omnest_sim.GetStatistics(
          "TestFalconNetwork.host[0].packet_builder.total_tx_packets") /
      kSimulationTime;
  LOG(INFO) << "packet_send_rate: " << packet_send_rate;

  // The offered load is 10Gbps, and the op size is 100 bytes. So the issued op
  // rate is about 12M pps. However, since the packet rate in PB is 10M pps, the
  // packet send rate should be 10M pps.
  EXPECT_DOUBLE_EQ(packet_send_rate, 10 * 1e6);
}

}  // namespace
}  // namespace isekai
