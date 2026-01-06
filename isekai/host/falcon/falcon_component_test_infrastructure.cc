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

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "glog/logging.h"
#include "gtest/gtest.h"
#include "isekai/common/common_util.h"
#include "isekai/common/common_util.h"  // IWYU pragma: keep
#include "isekai/common/config.pb.h"
#include "isekai/common/constants.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/net_address.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/resource_manager.h"
#include "isekai/host/falcon/gen2/falcon_model.h"
#include "isekai/host/falcon/gen3/falcon_model.h"
#include "isekai/host/rdma/rdma_component_interfaces.h"
#include "isekai/host/rnic/connection_manager.h"
#include "isekai/host/rnic/traffic_shaper_model.h"

namespace isekai {

// Helper methods for creating Read/Write requests.
std::unique_ptr<Packet> CreateWriteRequest(QpId src_qp, QpId dst_qp,
                                           uint32_t src_cid, uint32_t rsn,
                                           uint32_t op_size) {
  auto packet = std::make_unique<Packet>();
  packet->rdma.opcode = Packet::Rdma::Opcode::kWriteOnly;
  packet->rdma.dest_qp_id = dst_qp;
  packet->rdma.rsn = rsn;
  packet->rdma.sgl = {op_size};
  packet->rdma.data_length = kWriteHeaderSize;  // kCbth + kReth
  packet->rdma.request_length =
      kWriteHeaderSize + absl::c_accumulate(packet->rdma.sgl, 0U);
  packet->metadata.sgl_length =
      packet->rdma.sgl.size() * kSglFragmentHeaderSize;
  packet->metadata.rdma_src_qp_id = src_qp;
  packet->metadata.scid = src_cid;
  return packet;
}

std::unique_ptr<Packet> CreateReadRequest(QpId src_qp, QpId dst_qp,
                                          uint32_t src_cid, uint32_t rsn,
                                          uint32_t op_size) {
  auto packet = std::make_unique<Packet>();
  packet->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet->rdma.dest_qp_id = dst_qp;
  packet->rdma.rsn = rsn;
  packet->rdma.sgl = {op_size};
  packet->rdma.data_length =
      kReadHeaderSize;  // kCbth + kReth + kSeth + kSteth;
  packet->rdma.request_length =
      kResponseHeaderSize +
      absl::c_accumulate(packet->rdma.sgl, 0U);  // kCbth + kSteth;
  packet->metadata.rdma_src_qp_id = src_qp;
  packet->metadata.sgl_length =
      packet->rdma.sgl.size() * kSglFragmentHeaderSize;
  packet->metadata.scid = src_cid;
  return packet;
}

std::unique_ptr<Packet> CreateReadResponse(QpId src_qp, QpId dst_qp,
                                           uint32_t src_cid, uint32_t rsn,
                                           std::vector<uint32_t> sgl) {
  auto packet = std::make_unique<Packet>();
  packet->rdma.opcode = Packet::Rdma::Opcode::kReadResponseOnly;
  packet->rdma.dest_qp_id = dst_qp;
  packet->rdma.rsn = rsn;
  packet->rdma.sgl = std::move(sgl);
  packet->rdma.data_length = kResponseHeaderSize;  // kCbth + kSteth;
  packet->rdma.response_length =
      kResponseHeaderSize + absl::c_accumulate(packet->rdma.sgl, 0U);
  packet->metadata.sgl_length =
      packet->rdma.sgl.size() * kSglFragmentHeaderSize;
  packet->metadata.rdma_src_qp_id = src_qp;
  packet->metadata.scid = src_cid;
  return packet;
}

void FakeConnectionManager::InitializeHostIdToIpMap() {
  auto& host1_info = host_id_to_host_info_["host1"];
  host1_info.ip_address = kFalconHost1IpAddress;
  host1_info.next_local_cid = kHost1Scid;
  host1_info.next_local_qp_id = kHost1QpId;

  auto& host2_info = host_id_to_host_info_["host2"];
  host2_info.ip_address = kFalconHost2IpAddress;
  host2_info.next_local_cid = kHost2Scid;
  host2_info.next_local_qp_id = kHost2QpId;
}

void FakeRdma::HandleRxTransaction(std::unique_ptr<Packet> packet,
                                   std::unique_ptr<OpaqueCookie> cookie) {
  rx_packet_buffer_.push_back(std::move(packet));
}

void FakeRdma::HandleCompletion(QpId qp_id, uint32_t rsn,
                                Packet::Syndrome syndrome,
                                uint8_t destination_bifurcation_id) {
  completions_.push_back(std::make_pair(qp_id, rsn));
}

void FakeRdma::ReturnFalconCredit(QpId qp_id, const FalconCredit& credit) {
  remaining_rdma_managed_falcon_resources_ += credit;
}

void FakeRdma::DecrementFalconCredit(const Packet* packet) {
  auto work_scheduler = GetWorkSchedulerHandleForTesting();

  if (packet->rdma.opcode == Packet::Rdma::Opcode::kWriteOnly) {
    auto rdma_op = RdmaOp(RdmaOpcode::kWrite, packet->rdma.sgl, nullptr,
                          packet->rdma.dest_qp_id);
    remaining_rdma_managed_falcon_resources_ -=
        work_scheduler->ComputeRequestCredit(&rdma_op);
  } else if (packet->rdma.opcode == Packet::Rdma::Opcode::kReadRequest) {
    auto rdma_op = RdmaOp(RdmaOpcode::kRead, packet->rdma.sgl, nullptr,
                          packet->rdma.dest_qp_id);
    remaining_rdma_managed_falcon_resources_ -=
        work_scheduler->ComputeRequestCredit(&rdma_op);
  } else if (packet->rdma.opcode == Packet::Rdma::Opcode::kReadResponseOnly) {
    auto inbound_read_req =
        InboundReadRequest(packet->rdma.rsn, packet->rdma.sgl);
    remaining_rdma_managed_falcon_resources_ -=
        work_scheduler->ComputeResponseCredit(inbound_read_req);
  } else {
    LOG(FATAL)
        << " Component tests do not support other RDMA op types as of now";
  }
}

absl::StatusOr<std::unique_ptr<Packet>> FakeRdma::GetPacket() {
  if (rx_packet_buffer_.empty()) {
    return absl::InternalError("No rx packets.");
  }
  auto packet = std::move(rx_packet_buffer_.front());
  rx_packet_buffer_.pop_front();
  return packet;
}

absl::StatusOr<std::pair<QpId, uint32_t>> FakeRdma::GetCompletion() {
  if (completions_.empty()) {
    return absl::InternalError("No completions.");
  }
  std::pair<QpId, uint32_t> completion = completions_.front();
  completions_.pop_front();
  return completion;
}

FalconHost::FalconHost(int id, const Ipv6Address& ip, Environment* env,
                       PacketBuilderInterface* packet_builder,
                       ConnectionManagerInterface* connection_manager,
                       FalconConfig& falcon_config)
    : id_(id), host_id_(absl::StrCat("host-", id_)), ip_(ip) {
  RdmaConfig rdma_config = DefaultConfigGenerator::DefaultRdmaConfig();
  rdma_ = std::make_unique<FakeRdma>(rdma_config, env, connection_manager);
  connection_manager->RegisterRdma(absl::StrCat("host", id), rdma_.get());
  initial_rdma_managed_falcon_resources_ = {
      .request_tx_packet = rdma_config.global_credits().tx_packet_request(),
      .request_tx_buffer = rdma_config.global_credits().tx_buffer_request(),
      .request_rx_packet = rdma_config.global_credits().rx_packet_request(),
      .request_rx_buffer = rdma_config.global_credits().rx_buffer_request(),
      .response_tx_packet = rdma_config.global_credits().tx_packet_data(),
      .response_tx_buffer = rdma_config.global_credits().tx_buffer_data(),
  };
  traffic_shaper_ = std::make_unique<TrafficShaperModel>(
      DefaultConfigGenerator::DefaultTrafficShaperConfig(), env,
      /*stats_collector=*/nullptr);
  traffic_shaper_->ConnectPacketBuilder(packet_builder);

  falcon_config.set_threshold_solicit(256);
  if (falcon_config.version() == 1) {
    falcon_ = std::make_unique<FalconModel>(falcon_config, env,
                                            /*stats_collector=*/nullptr,
                                            connection_manager, host_id_,
                                            /* number of hosts = */ 4);
  } else if (falcon_config.version() == 2) {
    falcon_ = std::make_unique<Gen2FalconModel>(falcon_config, env,
                                                /*stats_collector=*/nullptr,
                                                connection_manager, host_id_,
                                                /* number of hosts = */ 4);
  } else if (falcon_config.version() == 3) {
    falcon_ = std::make_unique<Gen3FalconModel>(falcon_config, env,
                                                /*stats_collector=*/nullptr,
                                                connection_manager, host_id_,
                                                /* number of hosts = */ 4);
  }

  initial_falcon_resources_ =
      down_cast<ProtocolResourceManager*>(falcon_->get_resource_manager())
          ->GetAvailableResourceCreditsForTesting();
  falcon_->ConnectShaper(traffic_shaper_.get());
  falcon_->ConnectRdma(rdma_.get());
  connection_manager->RegisterFalcon(absl::StrCat("host", id), falcon_.get());
  rdma_->ConnectFalcon(falcon_.get());
}

void FalconHost::SubmitTxTransaction(std::unique_ptr<Packet> packet) {
  rdma_->DecrementFalconCredit(packet.get());
  falcon_->InitiateTransaction(std::move(packet));
}

std::unique_ptr<OpaqueCookie> FalconHost::CreateDefaultCookie() {
  std::unique_ptr<OpaqueCookie> cookie;
  if (falcon_->get_config()->version() == 1) {
    cookie = std::make_unique<OpaqueCookie>();
  } else if (falcon_->get_config()->version() >= 2) {
    cookie = std::make_unique<Gen2OpaqueCookie>();
  }
  return cookie;
}

void FalconHost::SubmitAckTransaction(uint32_t scid, uint32_t rsn,
                                      Packet::Syndrome ack_code,
                                      absl::Duration rnr_timeout) {
  falcon_->AckTransaction(scid, rsn, ack_code, rnr_timeout,
                          CreateDefaultCookie());
}

void FalconHost::SubmitAckTransactionWithNonDefaultCookie(
    uint32_t scid, uint32_t rsn, Packet::Syndrome ack_code,
    std::unique_ptr<OpaqueCookie> cookie, absl::Duration rnr_timeout) {
  falcon_->AckTransaction(scid, rsn, ack_code, rnr_timeout, std::move(cookie));
}

absl::StatusOr<std::unique_ptr<Packet>> FalconHost::GetSubmittedTransaction() {
  ASSIGN_OR_RETURN(auto packet, rdma_->GetPacket());
  return packet;
}

absl::StatusOr<std::pair<QpId, uint32_t>> FalconHost::GetCompletion() {
  return rdma_->GetCompletion();
}

// Checks if all Falcon resources were released. Used to ensure that releasing
// logic of resources works correctly.
bool FalconHost::CheckIfAllFalconResourcesReleasedForTesting() {
  auto current_available_resources =
      down_cast<ProtocolResourceManager*>(falcon_->get_resource_manager())
          ->GetAvailableResourceCreditsForTesting();
  return current_available_resources == initial_falcon_resources_;
}

// Checks if all RDMA managed Falcon resources were released. Used to ensure
// that releasing logic of resources works correctly.
bool FalconHost::CheckIfAllRdmaManagedFalconResourcesReleasedForTesting() {
  return rdma_->GetCurrentRdmaManagedFalconResources() ==
         initial_rdma_managed_falcon_resources_;
}

// Drops the first packet in packet_buffer_. Returns the information of the
// dropped packet.
std::string FakeNetwork::Drop() {
  if (packet_buffer_.empty()) {
    return "no packet is available to drop.";
  }
  auto dropped_packet = std::move(packet_buffer_.front());
  packet_buffer_.pop_front();
  return dropped_packet->DebugString();
}

// Drops packet with rsn of connection with dest_cid in packet_buffer_ . Returns
// the information of the dropped packet.
std::string FakeNetwork::DropByRsn(uint32_t dest_cid, uint32_t rsn) {
  std::string packet_info;
  for (auto it = packet_buffer_.begin(); it != packet_buffer_.end(); ++it) {
    if (it->get()->falcon.dest_cid == dest_cid &&
        it->get()->falcon.rsn == rsn) {
      packet_info = it->get()->DebugString();
      packet_buffer_.erase(it);
      break;
    }
  }
  return packet_info;
}

// Drops a range of packets in packet_buffer_ index range [start, end).
// Returns the number of packets dropped.
int FakeNetwork::DropMultipleWithIndexRange(uint32_t start, uint32_t end) {
  if (start > end) {
    LOG(FATAL) << "Dropping start index should always be lower than end.";
  }
  if (packet_buffer_.size() < end) {
    end = packet_buffer_.size();
  }
  packet_buffer_.erase(packet_buffer_.begin() + start,
                       packet_buffer_.begin() + end);
  return end - start;
}

bool FakeNetwork::Deliver(absl::Duration delay) {
  if (packet_buffer_.empty()) {
    return false;
  }

  if (delay == absl::ZeroDuration()) {
    auto delivered_packet = std::move(packet_buffer_.front());
    packet_buffer_.pop_front();
    delivered_packet->timestamps.received_timestamp = env_->ElapsedTime();
    if (delivered_packet->metadata.destination_ip_address == host2_->get_ip()) {
      host2_counter_tx_++;
      host2_->get_falcon()->TransferRxPacket(std::move(delivered_packet));
    } else {
      host1_counter_tx_++;
      host1_->get_falcon()->TransferRxPacket(std::move(delivered_packet));
    }
    return true;
  } else {
    CHECK_OK(env_->ScheduleEvent(delay,
                                 [this]() { Deliver(absl::ZeroDuration()); }));
    return true;
  }
}

void FakeNetwork::DeliverAll(absl::Duration delay) {
  int number_of_packets = packet_buffer_.size();
  // Delivers all packets in the network at uniform intervals within `delay`.
  for (int i = 1; i <= number_of_packets; i++) {
    CHECK_OK(env_->ScheduleEvent((i * delay) / number_of_packets,
                                 [this]() { Deliver(absl::ZeroDuration()); }));
  }
}

void FakeNetwork::InjectPacketToNetwork(std::unique_ptr<Packet> packet) {
  if (packet->metadata.destination_ip_address == host2_->get_ip()) {
    // Receives packet from host1.
    host1_counter_rx_++;
  } else {
    // Receives packet from host2.
    host2_counter_rx_++;
  }
  packet_buffer_.push_back(std::move(packet));
}

void FakeNetwork::ConnectFalconHost(FalconHost* host) {
  if (host->get_id() == 1) {
    host1_ = host;
  } else {
    host2_ = host;
  }
}

// Bring up Falcon hosts with the configuration being passed. In case the tests
// are parameterized on certain Falcon configurations, then the values passed to
// this function will be overridden by them. degree_of_multipathing (default
// value is 1) will set the degree of multipathing for the connection between
// host1 and host2.
void FalconComponentTest::InitFalconHosts(FalconConfig falcon_config,
                                          uint8_t degree_of_multipathing) {
  env_ = std::make_unique<SimpleEnvironment>();
  network_ = std::make_unique<FakeNetwork>(env_.get());
  packet_builder_.ConnectNetwork(network_.get());
  auto connection_manager = FakeConnectionManager();
  connection_manager.InitializeHostIdToIpMap();

  auto params = GetParam();
  falcon_config.set_resource_reservation_mode(testing::get<0>(params));
  falcon_config.set_inter_host_rx_scheduling_tick_ns(testing::get<1>(params));

  CHECK_OK_THEN_ASSIGN(auto ip_addr1,
                       Ipv6Address::OfString(kFalconHost1IpAddress));
  host1_ =
      std::make_unique<FalconHost>(1, ip_addr1, env_.get(), &packet_builder_,
                                   &connection_manager, falcon_config);
  network_->ConnectFalconHost(host1_.get());

  CHECK_OK_THEN_ASSIGN(auto ip_addr2,
                       Ipv6Address::OfString(kFalconHost2IpAddress));
  host2_ =
      std::make_unique<FalconHost>(2, ip_addr2, env_.get(), &packet_builder_,
                                   &connection_manager, falcon_config);
  network_->ConnectFalconHost(host2_.get());

  QpOptions qp_options;
  auto connection_options = std::make_unique<FalconConnectionOptions>();
  if (falcon_config.version() >= 2) {
    connection_options = std::make_unique<FalconMultipathConnectionOptions>(
        degree_of_multipathing);
  }
  RdmaConnectedMode rc_mode = RdmaConnectedMode::kOrderedRc;
  CHECK_OK(connection_manager
               .CreateConnection("host1", "host2", 0, 0, qp_options,
                                 *connection_options, rc_mode)
               .status())
      << "fail to initialize the connection between host1 and host2.";
}

int FalconComponentTest::GetFalconVersion() {
  return testing::get<2>(GetParam());
}

void FalconComponentTest::SubmitMultipleUnsolicitedWriteRequests(
    uint32_t start_rsn, int num_requests) {
  int rsn = start_rsn;
  for (int i = 0; i < num_requests; ++i) {
    auto request_packet = CreateWriteRequest(
        /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
        /*rsn=*/rsn++, /*op_size=*/32);
    host1_->SubmitTxTransaction(std::move(request_packet));
  }
  // Wait for Falcon processing all RDMA packets.
  env_->RunFor(num_requests *
               (kPerConnectionInterOpGap + kFalconProcessingDelay));
  // At this point, host1 should transmit num_message unsolicited data packets.
  ASSERT_EQ(network_->HeldPacketCount(), num_requests);
  for (int i = 0; i < num_requests; ++i) {
    const Packet* p = network_->PeekAt(i);
    ASSERT_EQ(p->packet_type, falcon::PacketType::kPushUnsolicitedData);
  }
}

void FalconComponentTest::TargetFalconDeliverPacketsToRdma(
    int num_packets, absl::Duration inter_host_rx_scheduler_tick) {
  // Host2 delivers num_transactions packets to RDMA.
  for (int i = 0; i < num_packets; ++i) {
    // RDMA on host2 should have received the transaction.
    ASSERT_OK_AND_ASSIGN(auto received_packet,
                         host2_->GetSubmittedTransaction());
    // Ack the Write request to Falcon host2 (target), and let it process.
    host2_->SubmitAckTransaction(kHost2Scid, received_packet->rdma.rsn,
                                 Packet::Syndrome::kAck);
    env_->RunFor(inter_host_rx_scheduler_tick + kFalconProcessingDelay);
  }
}

// Process of delivering single unsolicited push packet to host2, host2 RDMA
// receiving the packet, sending ACK back and host1 RDMA getting completion of
// the packet with the expected RSN.
void FalconComponentTest::
    DeliverPushUnsolicitedDataPacketAndCheckCompletionReceival(uint32_t rsn) {
  // Deliver data packet from host1 to host2 and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host2 should receive the write transaction with expected rsn.
  ASSERT_OK_AND_ASSIGN(auto received_packet, host2_->GetSubmittedTransaction());

  // Ack the Write request to Falcon host2 (target), and let it process.
  host2_->SubmitAckTransaction(kHost2Scid, received_packet->rdma.rsn,
                               Packet::Syndrome::kAck);
  env_->RunFor(kDefaultAckCoalescingTimer + kFalconProcessingDelay);

  // Next, host2 should transmit an ack packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p = network_->Peek();
  ASSERT_EQ(p->packet_type, falcon::PacketType::kAck);

  // Deliver ack from host2 to host1, and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host1 should get a completion from Falcon.
  ASSERT_OK_AND_ASSIGN(auto completion, host1_->GetCompletion());
  EXPECT_EQ(completion.first, kHost1QpId);
  EXPECT_EQ(completion.second, rsn);
}

}  // namespace isekai
