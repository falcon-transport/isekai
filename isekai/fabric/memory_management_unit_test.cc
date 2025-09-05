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

#include "isekai/fabric/memory_management_unit.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "inet/networklayer/common/L3Tools.h"
#include "inet/networklayer/ipv6/Ipv6Header_m.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/packet_m.h"
#include "isekai/fabric/constants.h"
#include "isekai/fabric/network_packet_queue.h"
#include "isekai/fabric/network_port.h"
#include "isekai/fabric/network_router.h"
#include "isekai/fabric/packet_util.h"
#include "isekai/fabric/routing_test_util.h"
#include "omnetpp/cmessage.h"
#undef ETH_ALEN
#undef ETHER_TYPE_LEN
#undef ETHERTYPE_ARP
#include "inet/linklayer/ethernet/EtherFrame_m.h"
#include "isekai/common/file_util.h"
#include "isekai/common/testing.h"
#include "omnetpp/cgate.h"

namespace isekai {

namespace {

static constexpr char kMmuConfigDataDir[] = "isekai/test_data/";

class FakeChannel : public omnetpp::cDatarateChannel {
 public:
  double getDatarate() const override { return 1e10; }
  omnetpp::simtime_t getTransmissionFinishTime() const override { return 0; }
};

class FakeGate : public omnetpp::cGate {};

class FakeQueue : public NetworkPacketQueue {
 public:
  FakeQueue(int queue_id, int port_id,
            MemoryManagementUnitInterface* memory_management_unit)
      // clang-format off
      : NetworkPacketQueue(queue_id, port_id, memory_management_unit,
                           nullptr) {}
  // clang-format on
  size_t Size() override { return 1; }
};

class FakePort : public NetworkPort {
 public:
  FakePort(NetworkRouterInterface* router, int port_id, int number_of_queues,
           RouterConfigProfile::ArbitrationScheme arbitration_scheme,
           StatisticCollectionInterface* stats_collection)
      : NetworkPort(router, port_id, number_of_queues, arbitration_scheme,
                    stats_collection, false) {
    queue_ = std::make_unique<FakeQueue>(0, 0, nullptr);
    channel_ = std::make_unique<FakeChannel>();
  }
  PacketQueueInterface* GetPacketQueue(int priority) const override {
    return queue_.get();
  }
  omnetpp::cDatarateChannel* GetTransmissionChannel() const override {
    return channel_.get();
  }

 private:
  std::unique_ptr<FakeQueue> queue_;
  std::unique_ptr<FakeChannel> channel_;
};

class FakeRouter : public NetworkRouter {
 public:
  FakeRouter() {
    port_ = std::make_unique<FakePort>(
        nullptr, 0, 0, RouterConfigProfile::FIXED_PRIORITY, nullptr);
    gate_ = std::make_unique<FakeGate>();
  }

  PortInterface* GetPort(int port_id) const override { return port_.get(); }

  bool IsPortConnected(int port_id) const override { return true; }

  FakeGate* gate(const char* gatename, int index) override {
    return gate_.get();
  }

  void scheduleAt(omnetpp::simtime_t t, omnetpp::cMessage* msg) override {
    auto event_type = static_cast<isekai::RouterEventType>(msg->getKind());
    switch (event_type) {
      case isekai::RouterEventType::kSetPfcPaused: {
        auto pfc_set_pause_msg = omnetpp::check_and_cast<PfcMessage*>(msg);
        memory_management_unit_->SetPfcPaused(
            pfc_set_pause_msg->getPortId(), pfc_set_pause_msg->getPriority(),
            omnetpp::simTime() +
                omnetpp::simtime_t(pfc_set_pause_msg->getPauseDuration(),
                                   omnetpp::SIMTIME_NS));
        break;
      }
      case isekai::RouterEventType::kPfcRefreshment: {
        // Do nothing.
        break;
      }
      case isekai::RouterEventType::kPfcPauseSend: {
        // Do nothing.
        break;
      }
      default: {
        LOG(ERROR) << "unknown message type.";
        delete msg;
        break;
      }
    }
  }

  void ConnectMmu(MemoryManagementUnit* memory_management_unit) {
    memory_management_unit_ = memory_management_unit;
  }

 private:
  std::unique_ptr<FakePort> port_;
  std::unique_ptr<FakeGate> gate_;
  MemoryManagementUnit* memory_management_unit_;
};

}  // namespace

TEST(MmuPfcTest, HandleXoffTest) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  FakeRouter router;
  MemoryManagementUnitConfig mmu_config;
  CHECK_OK(ReadTextProtoFromFile(
      absl::StrCat(kMmuConfigDataDir, "memory_management_config.pb.txt"),
      &mmu_config));
  MemoryManagementUnit mmu(&router, 1, kClassesOfService, mmu_config,
                           kDefaultNetworkMtuSize, nullptr);
  mmu.support_pfc_ = true;
  router.ConnectMmu(&mmu);

  EXPECT_TRUE(mmu.CanTransmit(0, 0));
  EXPECT_TRUE(mmu.CanTransmit(0, 3));

  // Set queue 0 to PAUSE.
  auto pfc_frame_0 = CreatePfcPacket(0, 1);
  mmu.ReceivedPfc(0, pfc_frame_0);
  EXPECT_FALSE(mmu.CanTransmit(0, 0));

  auto pfc_frame_1 = CreatePfcPacket(3, 1);
  mmu.ReceivedPfc(0, pfc_frame_1);
  EXPECT_FALSE(mmu.CanTransmit(0, 0));
  EXPECT_FALSE(mmu.CanTransmit(0, 3));

  delete pfc_frame_0;
  delete pfc_frame_1;
  CloseOmnestSimulation(dummy_simulator);
}

TEST(MmuPfcTest, HandleXonTest) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  FakeRouter router;
  MemoryManagementUnitConfig mmu_config;
  CHECK_OK(ReadTextProtoFromFile(
      absl::StrCat(kMmuConfigDataDir, "memory_management_config.pb.txt"),
      &mmu_config));
  MemoryManagementUnit mmu(&router, 1, kClassesOfService, mmu_config,
                           kDefaultNetworkMtuSize, nullptr);
  mmu.support_pfc_ = true;
  router.ConnectMmu(&mmu);

  auto pfc_frame_0 = CreatePfcPacket(0, 1);
  mmu.ReceivedPfc(0, pfc_frame_0);
  EXPECT_FALSE(mmu.CanTransmit(0, 0));

  auto pfc_frame_1 = CreatePfcPacket(0, 0);
  mmu.ReceivedPfc(0, pfc_frame_1);
  EXPECT_TRUE(mmu.CanTransmit(0, 0));

  delete pfc_frame_0;
  delete pfc_frame_1;
  CloseOmnestSimulation(dummy_simulator);
}

TEST(MmuPfcTest, TurnPfsomethingdOff) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  FakeRouter router;
  MemoryManagementUnitConfig mmu_config;
  CHECK_OK(ReadTextProtoFromFile(
      absl::StrCat(kMmuConfigDataDir, "memory_management_config.pb.txt"),
      &mmu_config));
  MemoryManagementUnit mmu(&router, 1, kClassesOfService, mmu_config,
                           kDefaultNetworkMtuSize, nullptr);
  mmu.support_pfc_ = true;
  router.ConnectMmu(&mmu);

  auto pfc_frame = CreatePfcPacket(3, 1);
  mmu.ReceivedPfc(0, pfc_frame);
  EXPECT_FALSE(mmu.CanTransmit(0, 3));

  mmu.support_pfc_ = false;
  EXPECT_TRUE(mmu.CanTransmit(0, 3));

  delete pfc_frame;
  CloseOmnestSimulation(dummy_simulator);
}

// The ingress threshold is determined by DetermineIngressLimit() - 1000 (we
// will update this once buffer carving feature is ready). In this unit test, we
// gradually increase the ingress counting by 100 at a time. Therefore, PFC will
// be triggered after receiving the 10th packet.
TEST(MmuPfcTest, TriggerXoffTest) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  FakeRouter router;
  MemoryManagementUnitConfig mmu_config;
  CHECK_OK(ReadTextProtoFromFile(
      absl::StrCat(kMmuConfigDataDir, "memory_management_config.pb.txt"),
      &mmu_config));
  MemoryManagementUnit mmu(&router, 1, kClassesOfService, mmu_config,
                           kDefaultNetworkMtuSize, nullptr);
  mmu.support_pfc_ = true;
  router.ConnectMmu(&mmu);

  auto port = static_cast<FakePort*>(mmu.router_->GetPort(0));
  port->port_state_ = PortState::kActive;
  mmu.ReceivedData(0, mmu.ingress_pool_free_cells_ / 2, 1);
  EXPECT_EQ(0, port->pfc_queue_.size());

  // Triggers PFC on port 0 queue 1.
  auto memory_occupancy = mmu.ReceivedData(0, 10, 1);
  EXPECT_EQ(1, port->pfc_queue_.size());
  EXPECT_FALSE(memory_occupancy.drop);
  EXPECT_EQ(0, memory_occupancy.minimal_guarantee_occupancy);
  EXPECT_EQ(0, memory_occupancy.shared_pool_occupancy);
  EXPECT_EQ(10, memory_occupancy.headroom_occupancy);
  EXPECT_EQ(10, mmu.per_port_per_queue_pfc_headroom_occupancy_[0][1]);

  mmu.ReceivedData(0, mmu.ingress_pool_free_cells_ / 2, 2);
  EXPECT_EQ(1, port->pfc_queue_.size());

  // Triggers PFC on port 0 queue 2.
  mmu.ReceivedData(0, 10, 2);
  EXPECT_EQ(1, port->pfc_queue_.size());

  auto pfc_frame = port->pfc_queue_.front();
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(pfc_frame);
  inet_packet->popAtFront<inet::EthernetMacHeader>();
  const auto& control_frame =
      inet_packet->peekAtFront<inet::EthernetPfcFrame>();

  EXPECT_EQ(control_frame->getClassEnable(), 6);
  EXPECT_NEAR(control_frame->getTimeClass(1) * kPauseUnitBits /
                  router.GetPort(0)->GetTransmissionChannel()->getDatarate(),
              1e-5, 1e-5);
  EXPECT_NEAR(control_frame->getTimeClass(2) * kPauseUnitBits /
                  router.GetPort(0)->GetTransmissionChannel()->getDatarate(),
              1e-5, 1e-5);

  port->pfc_queue_.pop();
  delete pfc_frame;
  CloseOmnestSimulation(dummy_simulator);
}

// In this unit test, we first trigger PFC by receiving a packet with size 1000
// (which eaquals the ingress threshold). Then, X-on message should be generated
// if we dequeue a packet, since the ingress counting will become smaller than
// the threshold again.
TEST(MmuPfcTest, TriggerXonTest) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  FakeRouter router;
  MemoryManagementUnitConfig mmu_config;
  CHECK_OK(ReadTextProtoFromFile(
      absl::StrCat(kMmuConfigDataDir, "memory_management_config.pb.txt"),
      &mmu_config));
  MemoryManagementUnit mmu(&router, 2, kClassesOfService, mmu_config,
                           kDefaultNetworkMtuSize, nullptr);
  mmu.support_pfc_ = true;
  router.ConnectMmu(&mmu);

  auto port = static_cast<FakePort*>(mmu.router_->GetPort(0));
  port->port_state_ = PortState::kActive;
  mmu.ReceivedData(0, mmu.ingress_pool_free_cells_ / 2, 1);
  mmu.ReceivedData(0, 10, 1);
  EXPECT_EQ(1, port->pfc_queue_.size());

  // Simulates the request enqueue process: the received packet (with size
  // mmu.ingress_pool_free_cells_ / 2) is processed by the routing pipeline, and
  // then enqueued into the tx queue. Once the packet is enqueued, the egress
  // occupancy counters like egress_pool_free_cells_ need to be updated
  // accordingly.
  mmu.egress_pool_free_cells_ -= mmu.ingress_pool_free_cells_ / 2;
  mmu.per_port_egress_occupancy_[1] += mmu.ingress_pool_free_cells_ / 2;
  mmu.per_port_per_queue_egress_occupancy_[1][1] +=
      mmu.ingress_pool_free_cells_ / 2;

  // Simulates the packet dequeue process. Assume the created falcon packets are
  // already enqueued in the tx queue.
  const auto& falcon_0 = inet::makeShared<FalconPacket>();
  falcon_0->setChunkLength(inet::units::values::B(256 * 9));
  inet::Packet* dequeue_packet_0 = new inet::Packet("FALCON");
  dequeue_packet_0->insertAtBack(falcon_0);
  // The payload is 9 cells, and the header will occupy 1 cell. So the total
  // size of dequeue_packet_0 is 10 cells.
  AddIngressTag(dequeue_packet_0, 0, 1, 0, 10, 0);
  auto ipv6_header_0 = inet::makeShared<inet::Ipv6Header>();
  ipv6_header_0->setExplicitCongestionNotification(0);
  ipv6_header_0->setChunkLength(
      inet::units::values::B(ipv6_header_0->calculateHeaderByteLength()));
  dequeue_packet_0->trimFront();
  inet::insertNetworkProtocolHeader(dequeue_packet_0, inet::Protocol::ipv6,
                                    ipv6_header_0);

  mmu.DequeuedPacket(1, 1, dequeue_packet_0);
  EXPECT_EQ(1, port->pfc_queue_.size());

  auto pfc_frame = port->pfc_queue_.front();
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(pfc_frame);
  inet_packet->popAtFront<inet::EthernetMacHeader>();
  const auto& control_frame =
      inet_packet->peekAtFront<inet::EthernetPfcFrame>();

  EXPECT_EQ(control_frame->getClassEnable(), 2);
  EXPECT_EQ(control_frame->getTimeClass(1), 0);

  port->pfc_queue_.pop();
  delete pfc_frame;

  const auto& falcon_1 = inet::makeShared<FalconPacket>();
  falcon_1->setChunkLength(inet::units::values::B(256));
  inet::Packet* dequeue_packet_1 = new inet::Packet("FALCON");
  dequeue_packet_1->insertAtBack(falcon_1);
  AddIngressTag(dequeue_packet_1, 0, 1, 0, 2, 0);
  auto ipv6_header_1 = inet::makeShared<inet::Ipv6Header>();
  ipv6_header_1->setExplicitCongestionNotification(0);
  ipv6_header_1->setChunkLength(
      inet::units::values::B(ipv6_header_1->calculateHeaderByteLength()));
  dequeue_packet_1->trimFront();
  inet::insertNetworkProtocolHeader(dequeue_packet_1, inet::Protocol::ipv6,
                                    ipv6_header_1);

  mmu.DequeuedPacket(1, 1, dequeue_packet_1);
  EXPECT_EQ(0, port->pfc_queue_.size());

  delete dequeue_packet_0;
  delete dequeue_packet_1;
  CloseOmnestSimulation(dummy_simulator);
}

TEST(MmuTest, IngressPacketDropTest) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  FakeRouter router;
  MemoryManagementUnitConfig mmu_config;
  CHECK_OK(ReadTextProtoFromFile(
      absl::StrCat(kMmuConfigDataDir, "memory_management_config.pb.txt"),
      &mmu_config));
  MemoryManagementUnit mmu(&router, 1, kClassesOfService, mmu_config,
                           kDefaultNetworkMtuSize, nullptr);
  mmu.support_pfc_ = false;
  router.ConnectMmu(&mmu);

  // PFC is disabled, no ingress packet will be dropped.
  EXPECT_FALSE(mmu.ReceivedData(0, mmu.ingress_pool_free_cells_, 1).drop);
  EXPECT_FALSE(mmu.ReceivedData(0, 1, 1).drop);

  mmu.support_pfc_ = true;
  mmu.pfc_sending_triggered_[0].set(1);
  // If PFC is supported and triggered, packet will be dropped if its size is
  // equal or larger than the pfc headroom size (100 in the current config).
  EXPECT_FALSE(mmu.ReceivedData(0, 100, 1).drop);

  CloseOmnestSimulation(dummy_simulator);
}

TEST(MmuTest, EgressPacketDropTest) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  FakeRouter router;
  MemoryManagementUnitConfig mmu_config;
  CHECK_OK(ReadTextProtoFromFile(
      absl::StrCat(kMmuConfigDataDir, "memory_management_config.pb.txt"),
      &mmu_config));
  MemoryManagementUnit mmu(&router, 1, kClassesOfService, mmu_config,
                           kDefaultNetworkMtuSize, nullptr);
  mmu.support_pfc_ = false;
  router.ConnectMmu(&mmu);

  // Create a dummy packet
  const auto& falcon_1 = inet::makeShared<FalconPacket>();
  falcon_1->setChunkLength(inet::units::values::B(4096));  // 4 KB = 16 cells
  std::unique_ptr<inet::Packet> falcon_packet =
      std::make_unique<inet::Packet>("FALCON");
  falcon_packet->insertAtBack(falcon_1);

  // Assuming all the packets are in priority 0
  uint32_t port_id = 0;
  int priority = 0;

  // Scenario 1: out of global buffer.
  mmu.per_port_egress_occupancy_[port_id] = 8;
  mmu.per_port_per_queue_egress_occupancy_[0][0] = 8;
  mmu.per_port_egress_alpha_[port_id] = 8.0;
  mmu.per_port_per_queue_egress_alpha_[port_id][priority] = 8.0;
  mmu.egress_pool_free_cells_ = 10;  // 10 free cells

  // Make sure falcon_packet size is 16 cells.
  EXPECT_EQ(GetPacketOccupancy(falcon_packet.get()), 16);

  // Expect packet drop because of free global buffer is smaller than the packet
  // size. egress_poll_free_cells_ (10) < packet_occupancy (16)
  EXPECT_FALSE(mmu.RequestEnqueue(port_id, priority, falcon_packet.get()));

  // Increase the global buffer to 16 with which we expect the packet can be
  // enqueued.
  mmu.egress_pool_free_cells_ = 16;
  EXPECT_TRUE(mmu.RequestEnqueue(port_id, priority, falcon_packet.get()));

  // Scenario 2: out of regional per-port buffer with alpha carving. We have
  // enough free cells (20) > packet size (16), but per-port buffer is 100.
  // With alpha (alpha = 8) carving, we expect the packet to be dropped even
  // though there are enough free cells.
  mmu.per_port_egress_occupancy_[port_id] = 160;
  mmu.per_port_per_queue_egress_occupancy_[port_id][priority] = 160;
  mmu.per_port_egress_alpha_[port_id] = 8.0;
  mmu.per_port_per_queue_egress_alpha_[port_id][priority] = 8.0;
  mmu.egress_pool_free_cells_ = 20;

  EXPECT_FALSE(mmu.RequestEnqueue(port_id, priority, falcon_packet.get()));

  // Decrease per-port occupancy to 100. We expect the packet to be enqueued.
  mmu.per_port_egress_occupancy_[port_id] = 100;
  mmu.per_port_per_queue_egress_occupancy_[0][0] = 100;
  EXPECT_TRUE(mmu.RequestEnqueue(port_id, priority, falcon_packet.get()));

  CloseOmnestSimulation(dummy_simulator);
}

}  // namespace isekai
