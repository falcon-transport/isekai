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

#include "isekai/host/falcon/falcon_component_test_infrastructure.h"

#include <memory>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "glog/logging.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/net_address.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/rnic/connection_manager.h"

namespace isekai {
namespace {

TEST(FalconComponentTestInfrastructureTest, TestFakeRdma) {
  SimpleEnvironment env;
  RdmaConfig rdma_config = DefaultConfigGenerator::DefaultRdmaConfig();
  FakeRdma rdma(rdma_config, &env, ConnectionManager::GetConnectionManager());

  auto tx_packet_1 = std::make_unique<Packet>();
  auto cookie1 = std::make_unique<OpaqueCookie>();
  tx_packet_1->rdma.rsn = 1;
  rdma.HandleRxTransaction(std::move(tx_packet_1), std::move(cookie1));

  auto tx_packet_2 = std::make_unique<Packet>();
  tx_packet_2->rdma.rsn = 2;
  auto cookie2 = std::make_unique<OpaqueCookie>();
  rdma.HandleRxTransaction(std::move(tx_packet_2), std::move(cookie2));

  auto rx_packet_1 = rdma.GetPacket();
  EXPECT_TRUE(rx_packet_1.ok());
  EXPECT_EQ(1, rx_packet_1.value()->rdma.rsn);

  auto rx_packet_2 = rdma.GetPacket();
  EXPECT_TRUE(rx_packet_2.ok());
  EXPECT_EQ(2, rx_packet_2.value()->rdma.rsn);
}

TEST(FalconComponentTestInfrastructureTest, TestFakeNetwork) {
  SimpleEnvironment env;
  FakeNetwork network(&env);
  auto* connection_manager = ConnectionManager::GetConnectionManager();

  FalconConfig falcon_config = DefaultConfigGenerator::DefaultFalconConfig(1);
  falcon_config.set_resource_reservation_mode(
      FalconConfig::FIRST_PHASE_RESERVATION);
  falcon_config.set_inter_host_rx_scheduling_tick_ns(0);

  FalconHost host1(1, Ipv6Address::OfString(kFalconHost1IpAddress).value(),
                   &env, nullptr, connection_manager, falcon_config);
  network.ConnectFalconHost(&host1);

  FalconHost host2(2, Ipv6Address::OfString(kFalconHost2IpAddress).value(),
                   &env, nullptr, connection_manager, falcon_config);
  network.ConnectFalconHost(&host2);

  auto tx_packet_1 = std::make_unique<Packet>();
  tx_packet_1->metadata.destination_ip_address =
      Ipv6Address::OfString(kFalconHost2IpAddress).value();
  auto tx_packet_1_info = tx_packet_1->DebugString();
  network.InjectPacketToNetwork(std::move(tx_packet_1));
  EXPECT_EQ(1, network.host1_counter_rx_);

  auto tx_packet_2 = std::make_unique<Packet>();
  tx_packet_2->metadata.destination_ip_address =
      Ipv6Address::OfString(kFalconHost1IpAddress).value();
  tx_packet_2->metadata.destination_bifurcation_id = 0;
  auto tx_packet_2_info = tx_packet_2->DebugString();
  network.InjectPacketToNetwork(std::move(tx_packet_2));
  EXPECT_EQ(1, network.host2_counter_rx_);

  auto dropped_packet_1_info = network.Drop();
  EXPECT_EQ(tx_packet_1_info, dropped_packet_1_info);

  auto dropped_packet_2_info = network.Drop();
  EXPECT_EQ(tx_packet_2_info, dropped_packet_2_info);

  EXPECT_FALSE(network.Deliver(absl::ZeroDuration()));
}

TEST(FalconComponentTestInfrastructureTest, TestDelayDelivery) {
  SimpleEnvironment env;
  FakeNetwork network(&env);
  auto connection_manager = FakeConnectionManager();

  // Establish the mapping between ID and IP address.
  connection_manager.InitializeHostIdToIpMap();

  FalconConfig falcon_config = DefaultConfigGenerator::DefaultFalconConfig(1);
  falcon_config.set_resource_reservation_mode(
      FalconConfig::FIRST_PHASE_RESERVATION);
  falcon_config.set_inter_host_rx_scheduling_tick_ns(0);

  FalconHost host1(1, Ipv6Address::OfString(kFalconHost1IpAddress).value(),
                   &env, nullptr, &connection_manager, falcon_config);
  network.ConnectFalconHost(&host1);

  FalconHost host2(2, Ipv6Address::OfString(kFalconHost2IpAddress).value(),
                   &env, nullptr, &connection_manager, falcon_config);
  network.ConnectFalconHost(&host2);

  // Establish connection between the hosts.
  QpOptions qp_options;
  FalconConnectionOptions connection_options;
  RdmaConnectedMode rc_mode = RdmaConnectedMode::kOrderedRc;
  ASSERT_OK(connection_manager
                .CreateConnection("host1", "host2", 0, 0, qp_options,
                                  connection_options, rc_mode)
                .status())
      << "fail to initialize the connection between host1 and host2.";

  auto tx_packet_1 = std::make_unique<Packet>();
  tx_packet_1->metadata.destination_ip_address =
      Ipv6Address::OfString(kFalconHost2IpAddress).value();
  tx_packet_1->packet_type = falcon::PacketType::kAck;
  tx_packet_1->ack.dest_cid = kHost2Scid;
  network.InjectPacketToNetwork(std::move(tx_packet_1));
  EXPECT_EQ(1, network.host1_counter_rx_);

  auto tx_packet_2 = std::make_unique<Packet>();
  tx_packet_2->metadata.destination_ip_address =
      Ipv6Address::OfString(kFalconHost1IpAddress).value();
  tx_packet_2->packet_type = falcon::PacketType::kAck;
  tx_packet_2->ack.dest_cid = kHost1Scid;
  network.InjectPacketToNetwork(std::move(tx_packet_2));
  EXPECT_EQ(1, network.host2_counter_rx_);

  EXPECT_TRUE(network.Deliver(absl::Nanoseconds(10)));
  EXPECT_TRUE(network.Deliver(absl::Nanoseconds(20)));

  EXPECT_EQ(0, network.host2_counter_tx_);
  env.RunFor(absl::Nanoseconds(10));
  EXPECT_EQ(1, network.host2_counter_tx_);

  EXPECT_EQ(0, network.host1_counter_tx_);
  env.RunFor(absl::Nanoseconds(10));
  EXPECT_EQ(1, network.host1_counter_tx_);
}

TEST(FalconComponentTestInfrastructureTest, TestNetworkPeekAt) {
  SimpleEnvironment env;
  FakeNetwork network(&env);
  auto connection_manager = FakeConnectionManager();

  // Establish the mapping between ID and IP address.
  connection_manager.InitializeHostIdToIpMap();

  FalconConfig falcon_config = DefaultConfigGenerator::DefaultFalconConfig(1);
  falcon_config.set_resource_reservation_mode(
      FalconConfig::FIRST_PHASE_RESERVATION);
  falcon_config.set_inter_host_rx_scheduling_tick_ns(0);

  FalconHost host1(1, Ipv6Address::OfString(kFalconHost1IpAddress).value(),
                   &env, nullptr, &connection_manager, falcon_config);
  network.ConnectFalconHost(&host1);

  FalconHost host2(2, Ipv6Address::OfString(kFalconHost2IpAddress).value(),
                   &env, nullptr, &connection_manager, falcon_config);
  network.ConnectFalconHost(&host2);

  // Establish connection between the hosts.
  QpOptions qp_options;
  FalconConnectionOptions connection_options;
  RdmaConnectedMode rc_mode = RdmaConnectedMode::kOrderedRc;
  ASSERT_OK(connection_manager
                .CreateConnection("host1", "host2", 0, 0, qp_options,
                                  connection_options, rc_mode)
                .status())
      << "fail to initialize the connection between host1 and host2.";

  auto tx_packet_1 = std::make_unique<Packet>();
  tx_packet_1->metadata.timestamp = 1;
  tx_packet_1->packet_type = falcon::PacketType::kAck;
  tx_packet_1->ack.dest_cid = kHost1Scid;
  tx_packet_1->metadata.destination_ip_address =
      Ipv6Address::OfString(kFalconHost1IpAddress).value();
  auto tx_packet_1_info = tx_packet_1->DebugString();
  network.InjectPacketToNetwork(std::move(tx_packet_1));

  auto tx_packet_2 = std::make_unique<Packet>();
  tx_packet_2->metadata.timestamp = 2;
  tx_packet_2->packet_type = falcon::PacketType::kAck;
  tx_packet_2->ack.dest_cid = kHost2Scid;
  tx_packet_2->metadata.destination_ip_address =
      Ipv6Address::OfString(kFalconHost2IpAddress).value();
  auto tx_packet_2_info = tx_packet_2->DebugString();
  network.InjectPacketToNetwork(std::move(tx_packet_2));

  auto tx_packet_3 = std::make_unique<Packet>();
  tx_packet_3->metadata.timestamp = 3;
  tx_packet_3->packet_type = falcon::PacketType::kAck;
  tx_packet_3->ack.dest_cid = kHost1Scid;
  tx_packet_3->metadata.destination_ip_address =
      Ipv6Address::OfString(kFalconHost1IpAddress).value();
  auto tx_packet_3_info = tx_packet_3->DebugString();
  network.InjectPacketToNetwork(std::move(tx_packet_3));

  // We should see all 3 packets in the network.
  EXPECT_EQ(network.PeekAt(0)->DebugString(), tx_packet_1_info);
  EXPECT_EQ(network.PeekAt(1)->DebugString(), tx_packet_2_info);
  EXPECT_EQ(network.PeekAt(2)->DebugString(), tx_packet_3_info);

  network.Deliver(absl::ZeroDuration());

  // After delivering one of them, other 2 should be visible.
  EXPECT_EQ(network.PeekAt(0)->DebugString(), tx_packet_2_info);
  EXPECT_EQ(network.PeekAt(1)->DebugString(), tx_packet_3_info);
}

TEST(FalconComponentTestInfrastructureTest, TestNetworkDeliverAll) {
  SimpleEnvironment env;
  FakeNetwork network(&env);
  auto connection_manager = FakeConnectionManager();

  // Establish the mapping between ID and IP address.
  connection_manager.InitializeHostIdToIpMap();

  FalconConfig falcon_config = DefaultConfigGenerator::DefaultFalconConfig(1);
  falcon_config.set_resource_reservation_mode(
      FalconConfig::FIRST_PHASE_RESERVATION);
  falcon_config.set_inter_host_rx_scheduling_tick_ns(0);

  FalconHost host1(1, Ipv6Address::OfString(kFalconHost1IpAddress).value(),
                   &env, nullptr, &connection_manager, falcon_config);
  network.ConnectFalconHost(&host1);

  FalconHost host2(2, Ipv6Address::OfString(kFalconHost2IpAddress).value(),
                   &env, nullptr, &connection_manager, falcon_config);
  network.ConnectFalconHost(&host2);

  // Establish connection between the hosts.
  QpOptions qp_options;
  FalconConnectionOptions connection_options;
  RdmaConnectedMode rc_mode = RdmaConnectedMode::kOrderedRc;
  ASSERT_OK(connection_manager
                .CreateConnection("host1", "host2", 0, 0, qp_options,
                                  connection_options, rc_mode)
                .status())
      << "fail to initialize the connection between host1 and host2.";

  auto tx_packet_1 = std::make_unique<Packet>();
  tx_packet_1->metadata.timestamp = 1;
  tx_packet_1->packet_type = falcon::PacketType::kAck;
  tx_packet_1->ack.dest_cid = kHost1Scid;
  tx_packet_1->metadata.destination_ip_address =
      Ipv6Address::OfString(kFalconHost1IpAddress).value();
  auto tx_packet_1_info = tx_packet_1->DebugString();
  network.InjectPacketToNetwork(std::move(tx_packet_1));

  auto tx_packet_2 = std::make_unique<Packet>();
  tx_packet_2->metadata.timestamp = 2;
  tx_packet_2->packet_type = falcon::PacketType::kAck;
  tx_packet_2->ack.dest_cid = kHost2Scid;
  tx_packet_2->metadata.destination_ip_address =
      Ipv6Address::OfString(kFalconHost2IpAddress).value();
  auto tx_packet_2_info = tx_packet_2->DebugString();
  network.InjectPacketToNetwork(std::move(tx_packet_2));

  auto tx_packet_3 = std::make_unique<Packet>();
  tx_packet_3->metadata.timestamp = 3;
  tx_packet_3->packet_type = falcon::PacketType::kAck;
  tx_packet_3->ack.dest_cid = kHost1Scid;
  tx_packet_3->metadata.destination_ip_address =
      Ipv6Address::OfString(kFalconHost1IpAddress).value();
  auto tx_packet_3_info = tx_packet_3->DebugString();
  network.InjectPacketToNetwork(std::move(tx_packet_3));

  network.DeliverAll(absl::Microseconds(10));
  env.RunFor(absl::Microseconds(11));

  EXPECT_EQ(network.host1_counter_rx_, 1);
  EXPECT_EQ(network.host2_counter_rx_, 2);
  EXPECT_EQ(network.host1_counter_tx_, 2);
  EXPECT_EQ(network.host2_counter_tx_, 1);
  EXPECT_FALSE(network.Deliver(absl::ZeroDuration()));
}

}  // namespace
}  // namespace isekai
