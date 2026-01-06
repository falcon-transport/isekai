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

#include "isekai/host/rnic/omnest_packet_builder.h"

#include <cstdint>
#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "inet/common/Protocol.h"
#include "inet/common/Units.h"
#include "inet/networklayer/common/L3Address.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/ecn_mark_tag_m.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/net_address.h"
#include "isekai/common/simple_environment.h"
#include "isekai/fabric/constants.h"
#include "isekai/fabric/packet_util.h"
#include "isekai/host/falcon/falcon.h"
#include "omnetpp/cdataratechannel.h"
#include "omnetpp/checkandcast.h"
#include "omnetpp/cmessage.h"
#include "omnetpp/simtime.h"
#include "omnetpp/simtime_t.h"

#undef NaN

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "inet/common/ProtocolTag_m.h"
#include "inet/common/Ptr.h"
#include "inet/common/packet/Packet.h"
#include "inet/linklayer/common/MacAddressTag_m.h"
#include "isekai/common/packet.h"
#include "isekai/common/packet_m.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/testing/integration/omnest_embedded_kernel.h"

#undef ETH_ALEN
#undef ETHER_TYPE_LEN
#include "inet/linklayer/ethernet/EtherPhyFrame_m.h"
#include "inet/networklayer/common/L3AddressTag_m.h"
#include "isekai/common/testing.h"

namespace isekai {

namespace {

class FakeChannel : public omnetpp::cDatarateChannel {
 public:
  explicit FakeChannel(Environment* env) : env_(env) {}
  double getDatarate() const override { return 1e10; }
  omnetpp::simtime_t getTransmissionFinishTime() const override {
    return omnetpp::SimTime(absl::ToDoubleSeconds(env_->ElapsedTime()));
  }

 private:
  Environment* const env_;
};

class FakeStatisticCollectionInterface : public StatisticCollectionInterface {
 public:
  absl::Status UpdateStatistic(
      std::string_view stats_output_name, double value,
      StatisticsCollectionConfig_StatisticsType output_type) override {
    scalar_stat_result_[stats_output_name] = value;
    return absl::OkStatus();
  };
  StatisticsCollectionConfig& GetConfig() override { return config; }

  double GetStatResult(absl::string_view stats_output_name) {
    return scalar_stat_result_[stats_output_name];
  }

 private:
  absl::flat_hash_map<std::string, double> scalar_stat_result_;
  StatisticsCollectionConfig config;
};

class FakeStatsCollection : public StatisticCollectionInterface {
 public:
  FakeStatsCollection() {}

  absl::Status UpdateStatistic(
      std::string_view stats_output_name, double value,
      StatisticsCollectionConfig_StatisticsType output_type) override {
    return absl::OkStatus();
  }
  StatisticsCollectionConfig& GetConfig() override { return config; }

 private:
  StatisticsCollectionConfig config;
};

}  // namespace

TEST(OmnestPacketBuilderTest, TestHashedPort) {
  OmnestEmbeddedKernel omnest_sim(/* ned_file_dir = */ kTestDir,
                                  /* ini_file = */ "dummy_omnetpp.ini",
                                  /* network_name = */ "DummyNetwork",
                                  /* simulation_time = */ 0);

  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ nullptr,
      /* env = */ nullptr,
      /* stats_collector = */ nullptr,
      /* ip_address = */ "::ffff:a00:2",
      /* transmission_channel = */ nullptr, /* host_id = */ "");

  std::string qp_ids = "1:1";
  std::string qp_ids_1 = "1:1";
  // Checks if the hashed port is deterministic.
  EXPECT_EQ(packet_builder.GetHashedPort(qp_ids),
            packet_builder.GetHashedPort(qp_ids_1));

  // Tests the probability of hash collision. Given N distinct qp ids, the
  // probability that at least one collision happens is 1 - e^(-N * (N - 1) / (2
  // * 2^16)). So the collision probability for 9 distinct qp ids is about
  // 0.0005, which is very low. This means we are likely to have 9 distinct
  // hashed ports.
  absl::flat_hash_set<uint16_t> hashed_ports;
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      auto qp_ids_ij = absl::StrCat(i, ":", j);
      auto hashed_port_ij = packet_builder.GetHashedPort(qp_ids_ij);
      hashed_ports.insert(hashed_port_ij);
    }
  }
  EXPECT_EQ(hashed_ports.size(), 9);
}

TEST(OmnestPacketBuilderTest, TestUdpHeader) {
  OmnestEmbeddedKernel omnest_sim(/* ned_file_dir = */ kTestDir,
                                  /* ini_file = */ "dummy_omnetpp.ini",
                                  /* network_name = */ "DummyNetwork",
                                  /* simulation_time = */ 0);

  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ nullptr,
      /* env = */ nullptr,
      /* stats_collector = */ nullptr,
      /* ip_address = */ "::ffff:a00:2",
      /* transmission_channel = */ nullptr, /* host_id = */ "");

  // Creates falcon_packet.
  auto falcon_packet = std::make_unique<Packet>();
  falcon_packet->falcon.psn = 100;
  falcon_packet->packet_type = falcon::PacketType::kPullData;
  falcon_packet->falcon.payload_length = 1000;

  // Create OMNest packet and put the content of falcon_packet into it.
  inet::Packet* packet = new inet::Packet("FALCON");
  const auto& falcon_content_original = inet::makeShared<FalconPacket>();
  falcon_content_original->setChunkLength(inet::units::values::b(
      packet_builder.GetFalconPacketLength(falcon_packet.get())));
  falcon_content_original->setFalconContent(*falcon_packet);
  packet->insertAtBack(falcon_content_original);
  // Inserts and removes Udp header and check if the extracted falcon_packet has
  // the same content as the original one.
  inet::L3Address src_ip_address("::ffff:a00:1");
  inet::L3Address dest_ip_address("::ffff:a00:2");
  packet_builder.InsertUdpHeader(packet, src_ip_address, dest_ip_address,
                                 kDefaultUdpPort);
  packet_builder.RemoveUdpHeader(packet);
  const auto& falcon_content_received =
      packet->peekAtFront<FalconPacket>()->getFalconContent();
  auto falcon_packet_received =
      std::make_unique<Packet>(falcon_content_received);

  EXPECT_EQ(100, falcon_packet_received->falcon.psn);

  delete packet;
}

TEST(OmnestPacketBuilderTest, TestIpHeader) {
  OmnestEmbeddedKernel omnest_sim(/* ned_file_dir = */ kTestDir,
                                  /* ini_file = */ "dummy_omnetpp.ini",
                                  /* network_name = */ "DummyNetwork",
                                  /* simulation_time = */ 0);
  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ nullptr,
      /* env = */ nullptr,
      /* stats_collector = */ nullptr,
      /* ip_address = */ "::ffff:a00:2",
      /* transmission_channel = */ nullptr, /* host_id = */ "");

  // Creates packet with UDP and IP headers.
  auto packet = packet_builder.CreatePacketWithUdpAndIpv6Headers(
      "::ffff:a00:1", "::ffff:a00:2", 1000, 1000);

  packet_builder.RemoveIpv6Header(packet.get());
  packet_builder.RemoveUdpHeader(packet.get());
  const auto& falcon_content_received =
      packet->peekAtFront<FalconPacket>()->getFalconContent();
  auto falcon_packet_received =
      std::make_unique<Packet>(falcon_content_received);

  EXPECT_EQ(kDefaultScid, falcon_packet_received->metadata.scid);
}

TEST(OmnestPacketBuilderTest, TestFrameHeader) {
  OmnestEmbeddedKernel omnest_sim(/* ned_file_dir = */ kTestDir,
                                  /* ini_file = */ "dummy_omnetpp.ini",
                                  /* network_name = */ "DummyNetwork",
                                  /* simulation_time = */ 0);

  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ nullptr,
      /* env = */ nullptr,
      /* stats_collector = */ nullptr,
      /* ip_address = */ "",
      /* transmission_channel = */ nullptr, /* host_id = */ "");

  // Creates falcon_packet.
  auto falcon_packet = std::make_unique<Packet>();
  falcon_packet->falcon.psn = 100;
  falcon_packet->packet_type = falcon::PacketType::kPullData;
  falcon_packet->falcon.payload_length = 1000;

  // Create OMNest packet and put the content of falcon_packet into it.
  inet::Packet* packet = new inet::Packet("FALCON");
  const auto& falcon_content_original = inet::makeShared<FalconPacket>();
  falcon_content_original->setChunkLength(inet::units::values::b(
      packet_builder.GetFalconPacketLength(falcon_packet.get())));
  falcon_content_original->setFalconContent(*falcon_packet);
  packet->insertAtBack(falcon_content_original);

  // Inserts and removes frame header and check if the extracted falcon_packet
  // has the same content as the original one.
  auto mac_address_req = packet->addTagIfAbsent<inet::MacAddressReq>();
  mac_address_req->setDestAddress(packet_builder.unique_mac_);
  mac_address_req->setSrcAddress(packet_builder.unique_mac_);
  packet->addTagIfAbsent<inet::PacketProtocolTag>()->setProtocol(
      &inet::Protocol::ipv6);
  packet_builder.InsertFrameHeader(packet);
  EXPECT_EQ(true, packet_builder.RemoveFrameHeader(packet));
  const auto& falcon_content_received =
      packet->peekAtFront<FalconPacket>()->getFalconContent();
  auto falcon_packet_received =
      std::make_unique<Packet>(falcon_content_received);

  EXPECT_EQ(100, falcon_packet_received->falcon.psn);

  delete packet;
}

TEST(OmnestPacketBuilderTest, TestPacketExtraction) {
  OmnestEmbeddedKernel omnest_sim(/* ned_file_dir = */ kTestDir,
                                  /* ini_file = */ "dummy_omnetpp.ini",
                                  /* network_name = */ "DummyNetwork",
                                  /* simulation_time = */ 0);

  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ nullptr,
      /* env = */ nullptr,
      /* stats_collector = */ nullptr,
      /* ip_address = */ "::ffff:a00:2",
      /* transmission_channel = */ nullptr, /* host_id = */ "");

  // Creates falcon_packet.
  auto packet = packet_builder.CreateFalconPacket("::ffff:a00:1",
                                                  "::ffff:a00:2", 1000, 1000);
  // The created falcon_packet does not contains phy header. Adds it back.
  const auto& phy_header = inet::makeShared<inet::EthernetPhyHeader>();
  phy_header->setSrcMacFullDuplex(true);
  packet->trim();
  packet->insertAtFront(phy_header);

  // Check if the kind of the generated packet is 0.
  EXPECT_EQ(packet->getKind(), 0);

  // Extracts frame header, IP and UDP header in sequece.
  EXPECT_EQ(true, packet_builder.RemoveFrameHeader(packet));
  packet_builder.RemoveIpv6Header(packet);
  EXPECT_EQ(true, packet_builder.RemoveUdpHeader(packet));

  // Checks if the extracted falcon_packet has the same content as the original
  // one.
  const auto& falcon_content_received =
      packet->peekAtFront<FalconPacket>()->getFalconContent();
  auto falcon_packet_received =
      std::make_unique<Packet>(falcon_content_received);
  EXPECT_EQ(100, falcon_packet_received->falcon.psn);
  EXPECT_EQ(absl::Nanoseconds(10),
            falcon_packet_received->timestamps.sent_timestamp);
  EXPECT_EQ(falcon::PacketType::kPushSolicitedData,
            falcon_packet_received->packet_type);

  delete packet;
}

TEST(OmnestPacketBuilderTest, TestPfcHandling) {
  OmnestEmbeddedKernel omnest_sim(/* ned_file_dir = */ kTestDir,
                                  /* ini_file = */ "dummy_omnetpp.ini",
                                  /* network_name = */ "DummyNetwork",
                                  /* simulation_time = */ 0);

  SimpleEnvironment env;
  FakeChannel channel(&env);
  int16_t pause_units = 10;
  auto pause_duration =
      absl::Seconds((pause_units * kPauseUnitBits) / channel.getDatarate());
  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ nullptr,
      /* env = */ &env,
      /* stats_collector = */ nullptr,
      /* ip_address = */ "",
      /* transmission_channel = */ &channel, /* host_id = */ "");
  packet_builder.support_pfc_ = true;
  EXPECT_EQ(packet_builder.packet_builder_queue_states_[0],
            PacketBuilderQueueState::kIdle);

  auto pfc_frame_0 = CreatePfcPacket(0, pause_units);
  auto inet_packet_0 = omnetpp::check_and_cast<inet::Packet*>(pfc_frame_0);
  const auto& phy_header_0 = inet::makeShared<inet::EthernetPhyHeader>();
  phy_header_0->setSrcMacFullDuplex(true);
  inet_packet_0->insertAtFront(phy_header_0);
  packet_builder.RemoveFrameHeader(inet_packet_0);

  // At t=0, received pfc_frame_0. Starts PFC pause after rx_pfc_delay_, and the
  // end time of the pause is rx_pfc_delay_ + pause_duration.
  env.RunFor(packet_builder.rx_pfc_delay_);
  EXPECT_EQ(packet_builder.packet_builder_queue_states_[0],
            PacketBuilderQueueState::kPaused);

  auto pfc_frame_1 = CreatePfcPacket(0, pause_units);
  auto inet_packet_1 = omnetpp::check_and_cast<inet::Packet*>(pfc_frame_1);
  const auto& phy_header_1 = inet::makeShared<inet::EthernetPhyHeader>();
  phy_header_1->setSrcMacFullDuplex(true);
  inet_packet_1->insertAtFront(phy_header_1);
  packet_builder.RemoveFrameHeader(inet_packet_1);

  // At t = rx_pfc_delay_, received pfc_frame_1. Starts PFC pause at time 2 *
  // rx_pfc_delay_. The pause end time is then updated to 2 * rx_pfc_delay_ +
  // pause_duration.
  env.RunFor(pause_duration);
  EXPECT_EQ(packet_builder.packet_builder_queue_states_[0],
            PacketBuilderQueueState::kPaused);

  env.RunFor(packet_builder.rx_pfc_delay_);
  EXPECT_EQ(packet_builder.packet_builder_queue_states_[0],
            PacketBuilderQueueState::kIdle);
  delete pfc_frame_0;
  delete pfc_frame_1;
}

TEST(OmnestPacketBuilderTest, TestEcnTag) {
  OmnestEmbeddedKernel omnest_sim(/* ned_file_dir = */ kTestDir,
                                  /* ini_file = */ "dummy_omnetpp.ini",
                                  /* network_name = */ "DummyNetwork",
                                  /* simulation_time = */ 0);
  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ nullptr,
      /* env = */ nullptr,
      /* stats_collector = */ nullptr,
      /* ip_address = */ "::ffff:a00:2",
      /* transmission_channel = */ nullptr, /* host_id = */ "");

  auto packet = packet_builder.CreateFalconPacket("::ffff:a00:1",
                                                  "::ffff:a00:2", 1000, 1000);
  DecapsulateMacHeader(packet);
  PacketSetEcn(packet, EcnCode::kCongestionEncountered);
  EXPECT_EQ(EcnCode::kCongestionEncountered, PacketGetEcn(packet));

  packet_builder.RemoveIpv6Header(packet);
  packet_builder.RemoveUdpHeader(packet);
  auto ecn_mark_tag = packet->removeTagIfPresent<EcnMarkTag>();
  EXPECT_TRUE(ecn_mark_tag->getEcnMarked());

  delete ecn_mark_tag;
  delete packet;
}

TEST(OmnestPacketBuilderTest, TestWrongPacketSize) {
  OmnestEmbeddedKernel omnest_sim(/* ned_file_dir = */ kTestDir,
                                  /* ini_file = */ "dummy_omnetpp.ini",
                                  /* network_name = */ "DummyNetwork",
                                  /* simulation_time = */ 0);
  SimpleEnvironment env;
  FakeStatsCollection stats_collection;
  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ nullptr,
      /* env = */ &env,
      /* stats_collector = */ &stats_collection,
      /* ip_address = */ "::ffff:a00:2",
      /* transmission_channel = */ nullptr, /* host_id = */ "");
  packet_builder.stats_collection_flags_.set_enable_discard_and_drops(true);

  // Creates a FALCON packet with 5KB payload.
  auto packet = packet_builder.CreateFalconPacket(
      /* src_ip_address = */ "::ffff:a00:1",
      /* dest_ip_address= */ "::ffff:a00:2",
      /* src_port= */ 1000, /* dest_port = */ 1000, /* packet_payload= */ 5120);
  // The created falcon_packet does not contains phy header. Adds it back.
  const auto& phy_header = inet::makeShared<inet::EthernetPhyHeader>();
  phy_header->setSrcMacFullDuplex(true);
  packet->trim();
  packet->insertAtFront(phy_header);
  packet_builder.SendoutPacket(packet);
  EXPECT_EQ(packet_builder.wrong_outgoing_packet_size_discards_, 1);
}

TEST(OmnestPacketBuilderTest, TestTriggerXonAndXoff) {
  OmnestEmbeddedKernel omnest_sim(/* ned_file_dir = */ kTestDir,
                                  /* ini_file = */ "dummy_omnetpp.ini",
                                  /* network_name = */ "DummyNetwork",
                                  /* simulation_time = */ 0);
  FalconModel falcon(DefaultConfigGenerator::DefaultFalconConfig(1), nullptr,
                     nullptr, nullptr, "", /* number of hosts */ 4);
  SimpleEnvironment env;
  FakeChannel channel(&env);
  FakeStatisticCollectionInterface stats_collection;
  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ &falcon,
      /* env = */ &env,
      /* stats_collector = */ &stats_collection,
      /* ip_address = */ "::ffff:a00:2",
      /* transmission_channel = */ &channel,
      /* host_id = */ "");
  packet_builder.tx_queue_length_threshold_ = 10;
  packet_builder.stats_collection_flags_.set_enable_xoff_duration(true);

  std::string dst_ip = "2001:700:300:1800::f";
  // The first packet will be sent out, and the left 9 packets will be stored in
  // TX queue.
  for (int i = 0; i < 10; i++) {
    auto falcon_packet = std::make_unique<Packet>();
    falcon_packet->packet_type = falcon::PacketType::kPushSolicitedData;
    falcon_packet->falcon.payload_length = 1000;
    ASSERT_OK_AND_ASSIGN(falcon_packet->metadata.destination_ip_address,
                         Ipv6Address::OfString(dst_ip));
    packet_builder.EnqueuePacket(std::move(falcon_packet));
  }
  EXPECT_TRUE(falcon.CanSendPacket());

  // After enqueuing falcon_packet_1, the TX queue size becomes 10, reaching its
  // threshold. So Xoff is triggered.
  auto falcon_packet_1 = std::make_unique<Packet>();
  falcon_packet_1->packet_type = falcon::PacketType::kPushSolicitedData;
  falcon_packet_1->falcon.payload_length = 1000;
  ASSERT_OK_AND_ASSIGN(falcon_packet_1->metadata.destination_ip_address,
                       Ipv6Address::OfString(dst_ip));
  packet_builder.EnqueuePacket(std::move(falcon_packet_1));
  EXPECT_FALSE(falcon.CanSendPacket());

  // TX queue has reached its limit after enqueuing falcon_packet_1. So
  // falcon_packet_2 will be enqueued in the logical headroom.
  auto falcon_packet_2 = std::make_unique<Packet>();
  falcon_packet_2->packet_type = falcon::PacketType::kPushSolicitedData;
  falcon_packet_2->falcon.payload_length = 1000;
  ASSERT_OK_AND_ASSIGN(falcon_packet_2->metadata.destination_ip_address,
                       Ipv6Address::OfString(dst_ip));
  packet_builder.EnqueuePacket(std::move(falcon_packet_2));
  EXPECT_FALSE(falcon.CanSendPacket());
  EXPECT_EQ(packet_builder.tx_queue_.size(), 11);

  absl::Duration time_duration = absl::ZeroDuration();
  absl::Duration next_pb_event_time;
  // Dequeues a packet from TX queue, the TX queue size is 10, which still can
  // not process new packets
  env.RunFor(next_pb_event_time = packet_builder.packet_build_interval_);
  time_duration += next_pb_event_time;
  EXPECT_FALSE(falcon.CanSendPacket());

  // Dequeues one more packet from TX queue, the TX queue size becomes 9, which
  // is smaller than its threshold. Triggers Xon in FALCON.
  env.RunFor(next_pb_event_time = packet_builder.packet_build_interval_);
  EXPECT_TRUE(falcon.CanSendPacket());
  time_duration += next_pb_event_time;
  EXPECT_EQ(
      stats_collection.GetStatResult("packet_builder.total_xoff_duration"),
      absl::ToDoubleMicroseconds(time_duration));
}

TEST(OmnestPacketBuilderTest, TestFlowLabel) {
  const std::string ip_src_address = "::ffff:a00:2";
  const std::string ip_dst_ip = "2001:700:300:1800::f";

  OmnestEmbeddedKernel omnest_sim(/* ned_file_dir = */ kTestDir,
                                  /* ini_file = */ "dummy_omnetpp.ini",
                                  /* network_name = */ "DummyNetwork",
                                  /* simulation_time = */ 0);

  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ nullptr,
      /* env = */ nullptr,
      /* stats_collector = */ nullptr,
      /* ip_address = */ ip_src_address,
      /* transmission_channel = */ nullptr, /* host_id = */ "");

  // Creates falcon_packet.
  auto falcon_packet = std::make_unique<Packet>();
  falcon_packet->metadata.flow_label = kDefaultFlowLabel;
  falcon_packet->packet_type = falcon::PacketType::kPushSolicitedData;
  falcon_packet->falcon.payload_length = 1000;
  // Create OMNest packet and put the content of falcon_packet into it.
  auto packet = std::make_unique<inet::Packet>("FALCON");
  const auto& falcon_content_original = inet::makeShared<FalconPacket>();
  falcon_content_original->setChunkLength(inet::units::values::B(
      packet_builder.GetFalconPacketLength(falcon_packet.get())));
  falcon_content_original->setFalconContent(*falcon_packet);
  packet->insertAtBack(falcon_content_original);

  packet_builder.InsertUdpHeader(
      /*packet=*/packet.get(),
      /*source_address=*/inet::L3Address(ip_src_address.c_str()),
      /*destination_address=*/inet::L3Address(ip_dst_ip.c_str()),
      /*source_port=*/kDefaultUdpPort,
      /*destination_port=*/kDefaultUdpPort);

  EXPECT_EQ(packet_builder.GetFlowLabelFromFalconMetadata(packet.get()),
            kDefaultFlowLabel);
}

TEST(OmnestPacketBuilderTest, TestRandomDrop) {
  OmnestEmbeddedKernel omnest_sim(/* ned_file_dir = */ kTestDir,
                                  /* ini_file = */ "dummy_omnetpp.ini",
                                  /* network_name = */ "DummyNetwork",
                                  /* simulation_time = */ 0);
  SimpleEnvironment env;
  FakeStatsCollection stats_collection;
  int n = 10000;

  for (double drop_probability = 0; drop_probability < 1;
       drop_probability += 0.1) {
    for (double drop_burst_size = 1; drop_burst_size < 10;
         drop_burst_size += 1) {
      // Re-instantiate packet_builder for each drop_probability.
      OmnestPacketBuilder packet_builder(
          /* host_module = */ nullptr,
          /* falcon = */ nullptr,
          /* env = */ &env,
          /* stats_collector = */ &stats_collection,
          /* ip_address = */ "::ffff:a00:2",
          /* transmission_channel = */ nullptr, /* host_id = */ "");

      packet_builder.stats_collection_flags_.set_enable_discard_and_drops(true);
      packet_builder.SetRandomDropProbability(drop_probability);
      packet_builder.drop_burst_size_ = drop_burst_size;

      for (int i = 0; i < n; i++) {
        // Creates a FALCON packet with 5KB payload.
        auto packet = packet_builder.CreateFalconPacket(
            /* src_ip_address = */ "::ffff:a00:1",
            /* dest_ip_address= */ "::ffff:a00:2",
            /* src_port= */ 1000, /* dest_port = */ 1000,
            /* packet_payload= */ 512);
        // The created falcon_packet does not contains phy header. Adds it back.
        const auto& phy_header = inet::makeShared<inet::EthernetPhyHeader>();
        phy_header->setSrcMacFullDuplex(true);
        packet->trim();
        packet->insertAtFront(phy_header);
        packet_builder.SendoutPacket(packet);
      }
      if (drop_probability == 0.0) {
        EXPECT_EQ(packet_builder.random_drops_, 0);
      } else {
        EXPECT_LE(packet_builder.random_drops_, n * (drop_probability + 0.1));
        EXPECT_GE(packet_builder.random_drops_, n * (drop_probability - 0.1));
      }
    }
  }
}

}  // namespace isekai
