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

#include "gtest/gtest.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"

namespace isekai {
namespace {

TEST(FalconModel, TestUpdateRxBytes) {
  // Creates a falcon model.
  auto config = DefaultConfigGenerator::DefaultFalconConfig(1);
  FalconModel falcon(config, nullptr, nullptr, nullptr, "",
                     /* number of hosts */ 4);

  // Creates falcon_packet.
  uint32_t cid = 5;
  uint32_t pkt_size = 1500;
  auto falcon_packet = std::make_unique<Packet>();
  falcon_packet->falcon.psn = 100;
  falcon_packet->packet_type = falcon::PacketType::kPullData;
  falcon_packet->falcon.payload_length = 1000;
  falcon_packet->falcon.dest_cid = cid;

  // Updates per connection byte counter in falcon model.
  falcon.UpdateRxBytes(std::move(falcon_packet), pkt_size);

  // Checks if the correct amount of bytes are added.
  EXPECT_EQ(falcon.get_stats_manager()->GetConnectionCounters(cid).rx_bytes,
            pkt_size);

  // Checks if non-existed connection has any rx bytes.
  EXPECT_EQ(falcon.get_stats_manager()->GetConnectionCounters(27).rx_bytes, 0);

  // Creates another packet with the same connection id.
  falcon_packet = std::make_unique<Packet>();
  falcon_packet->falcon.dest_cid = cid;

  // Checks if the rx bytes of the same connection are incremented correctly.
  falcon.UpdateRxBytes(std::move(falcon_packet), 600);
  EXPECT_EQ(falcon.get_stats_manager()->GetConnectionCounters(cid).rx_bytes,
            pkt_size + 600);
}

TEST(FalconModel, TestUpdateTxBytes) {
  // Creates a falcon model.
  auto config = DefaultConfigGenerator::DefaultFalconConfig(1);
  FalconModel falcon(config, nullptr, nullptr, nullptr, "",
                     /* number of hosts */ 4);

  // Creates falcon_packet.
  const uint32_t cid = 5;
  const uint32_t pkt_size = 1500;
  auto falcon_packet = std::make_unique<Packet>();
  falcon_packet->packet_type = falcon::PacketType::kPullData;
  falcon_packet->falcon.dest_cid = cid;

  // Updates per connection byte counter in falcon model.
  falcon.UpdateTxBytes(std::move(falcon_packet), pkt_size);

  // Checks if the correct amount of bytes are added.
  EXPECT_EQ(falcon.get_stats_manager()->GetConnectionCounters(cid).tx_bytes,
            pkt_size);

  // Checks if non-existed connection has any rx bytes.
  EXPECT_EQ(falcon.get_stats_manager()->GetConnectionCounters(27).tx_bytes, 0);

  const uint32_t pkt_size1 = 600;
  // Creates another packet with the same connection id.
  falcon_packet = std::make_unique<Packet>();
  falcon_packet->falcon.dest_cid = cid;

  // Checks if the rx bytes of the same connection are incremented correctly.
  falcon.UpdateTxBytes(std::move(falcon_packet), pkt_size1);
  EXPECT_EQ(falcon.get_stats_manager()->GetConnectionCounters(cid).tx_bytes,
            pkt_size + pkt_size1);
}

TEST(FalconModel, TestGetFalconPacketConnectionId) {
  // Creates a non-ack and non-nack falcon packet.
  uint32_t cid = 1;
  auto falcon_packet = std::make_unique<Packet>();
  falcon_packet->packet_type = falcon::PacketType::kPullData;
  falcon_packet->falcon.dest_cid = cid;
  EXPECT_EQ(GetFalconPacketConnectionId(*falcon_packet), cid);

  // Creates an ack falcon packet.
  cid = 2;
  falcon_packet = std::make_unique<Packet>();
  falcon_packet->packet_type = falcon::PacketType::kAck;
  falcon_packet->ack.dest_cid = cid;
  EXPECT_EQ(GetFalconPacketConnectionId(*falcon_packet), cid);

  // Creates a nack falcon packet.
  cid = 3;
  falcon_packet = std::make_unique<Packet>();
  falcon_packet->packet_type = falcon::PacketType::kNack;
  falcon_packet->nack.dest_cid = cid;
  EXPECT_EQ(GetFalconPacketConnectionId(*falcon_packet), cid);
}

}  // namespace
}  // namespace isekai
