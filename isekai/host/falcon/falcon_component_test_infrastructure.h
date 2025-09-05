/*
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ISEKAI_HOST_FALCON_FALCON_COMPONENT_TEST_INFRASTRUCTURE_H_
#define ISEKAI_HOST_FALCON_FALCON_COMPONENT_TEST_INFRASTRUCTURE_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/net_address.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/host/falcon/falcon_resource_credits.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/rdma/rdma_falcon_model.h"
#include "isekai/host/rnic/connection_manager.h"
#include "isekai/host/rnic/traffic_shaper_model.h"
#include "omnetpp/cmessage.h"

namespace isekai {

// A FIFO stores sent-out packets.
using NetworkPacketBuffer = std::deque<std::unique_ptr<Packet>>;
// The ip address of host1/2.
constexpr char kFalconHost1IpAddress[] = "2001:db8:85a2::1";
constexpr char kFalconHost2IpAddress[] = "2001:db8:85a3::1";
// Total number of hosts.
constexpr uint8_t kNumberOfHosts = 4;
// The scid and qp_id for host1 and host2.
constexpr QpId kHost1QpId = 101;
constexpr QpId kHost2QpId = 201;
constexpr uint32_t kHost1Scid = 1001;
constexpr uint32_t kHost2Scid = 2001;

// A list of configured delays to run the simulation for a sufficient amount of
// time in order to observe desired effect. These are based on default values in
// FalconConfig and FalconConnectionState.

// Max time taken by FALCON to process a packet from the network or a request
// from RDMA. After this delay, we expect FALCON to either send a
// transaction/completion to RDMA or transmit a packet to the network. It should
// also be lower than any retransmit timers to prevent duplicate actions by
// FALCON.
const absl::Duration kFalconProcessingDelay =
    2 *
    absl::Nanoseconds(
        DefaultConfigGenerator::DefaultFalconConfig(1).falcon_tick_time_ns());

// Time taken by RDMA to process a request and return a response.
const absl::Duration kRdmaProcessingDelay = absl::Nanoseconds(
    DefaultConfigGenerator::DefaultRdmaConfig().chip_cycle_time_ns());

// Max time taken by FALCON to detect retransmission timeout and send out a
// retransmission packet. Note this includes the timeout itself.
constexpr absl::Duration kRetransmissionDelay = absl::Milliseconds(1);

// Default one-way network delay.
constexpr absl::Duration kNetworkDelay = absl::Microseconds(10);

// Default RNR Timeout delay.
constexpr absl::Duration kRnrTimeout = absl::Microseconds(1);

// Starting RSN for both RDMA and FALCON.
const uint32_t kRsn = 0;

// Helper methods for creating Read/Write requests.
std::unique_ptr<Packet> CreateWriteRequest(QpId src_qp, QpId dst_qp,
                                           uint32_t src_cid, uint32_t rsn,
                                           uint32_t op_size);

std::unique_ptr<Packet> CreateReadRequest(QpId src_qp, QpId dst_qp,
                                          uint32_t src_cid, uint32_t rsn,
                                          uint32_t op_size);

std::unique_ptr<Packet> CreateReadResponse(QpId src_qp, QpId dst_qp,
                                           uint32_t src_cid, uint32_t rsn,
                                           std::vector<uint32_t> sgl);

// A fake connection manager, which is a friend class of ConnectionManager, to
// provide connection management for host1 and host2 in FALCON component test.
class FakeConnectionManager : public ConnectionManager {
 public:
  void InitializeHostIdToIpMap();
};

// A fake RDMA model that can store the a packet from FALCON to RDMA in a
// buffer. The FALCON host will later get the stored packet by calling
// GetSubmittedTransaction().
class FakeRdma : public RdmaFalconModel {
 public:
  FakeRdma(const RdmaConfig& config, Environment* env,
           ConnectionManagerInterface* connection_manager)
      : RdmaFalconModel(config, env,
                        /*stats_collector=*/nullptr, connection_manager) {
    remaining_rdma_managed_falcon_resources_ = {
        .request_tx_packet = config.global_credits().tx_packet_request(),
        .request_tx_buffer = config.global_credits().tx_buffer_request(),
        .request_rx_packet = config.global_credits().rx_packet_request(),
        .request_rx_buffer = config.global_credits().rx_buffer_request(),
        .response_tx_packet = config.global_credits().tx_packet_data(),
        .response_tx_buffer = config.global_credits().tx_buffer_data(),
    };
  }
  void HandleRxTransaction(std::unique_ptr<Packet> packet,
                           std::unique_ptr<OpaqueCookie> cookie) override;
  void HandleCompletion(QpId qp_id, uint32_t rsn, Packet::Syndrome syndrome,
                        uint8_t destination_bifurcation_id) override;
  void ReturnFalconCredit(QpId qp_id, const FalconCredit& credit) override;
  // Fake method to decrement FALCON credits when RDMA hands over transactions
  // to FALCON.
  void DecrementFalconCredit(const Packet* packet);
  // Methods to get packets/completions/credits out of FakeRdma.
  absl::StatusOr<std::unique_ptr<Packet>> GetPacket();
  absl::StatusOr<std::pair<QpId, uint32_t>> GetCompletion();
  FalconCredit GetCurrentRdmaManagedFalconResources() {
    return remaining_rdma_managed_falcon_resources_;
  }

 private:
  NetworkPacketBuffer rx_packet_buffer_;
  std::deque<std::pair<QpId, uint32_t>> completions_;
  FalconCredit remaining_rdma_managed_falcon_resources_;
};

// The FALCON host includes fake RDMA and FALCON models. The fake RDMA is
// included in order to allow users ack to FALCON model.
class FalconHost {
 public:
  FalconHost(int id, const Ipv6Address& ip, Environment* env,
             PacketBuilderInterface* packet_builder,
             ConnectionManagerInterface* connection_manager,
             FalconConfig& falcon_config);
  // Submits a TX transaction and send it to the network.
  void SubmitTxTransaction(std::unique_ptr<Packet> packet);
  // Creates a default-initialized ULP cookie of the right generation (i.e.,
  // Gen1, Gen2).
  std::unique_ptr<OpaqueCookie> CreateDefaultCookie();
  // Submits an ACK transaction with the default-value cookie and send it to the
  // network.
  void SubmitAckTransaction(uint32_t scid, uint32_t rsn,
                            Packet::Syndrome ack_code,
                            absl::Duration rnr_timeout = absl::ZeroDuration());
  // Submits an ACK transaction with a non-default cookie and send it to the
  // network.
  void SubmitAckTransactionWithNonDefaultCookie(
      uint32_t scid, uint32_t rsn, Packet::Syndrome ack_code,
      std::unique_ptr<OpaqueCookie> cookie,
      absl::Duration rnr_timeout = absl::ZeroDuration());
  // Gets the submitted TX / ACK transaction from the network. The user should
  // process the received transaction and take the corresponding actions.
  absl::StatusOr<std::unique_ptr<Packet>> GetSubmittedTransaction();
  // Gets completions from the RDMA model. A completion is identified by a
  // {QpId, rsn} tuple.
  absl::StatusOr<std::pair<QpId, uint32_t>> GetCompletion();
  // Getter to get the host ip/id address.
  const Ipv6Address& get_ip() const { return ip_; }
  int get_id() const { return id_; }
  FalconModel* get_falcon() const { return falcon_.get(); }
  void SetFalconXoff(bool xoff) { falcon_->SetXoffByPacketBuilder(xoff); }
  // Checks if all FALCON resources were released. Used to ensure that releasing
  // logic of resources works correctly.
  bool CheckIfAllFalconResourcesReleasedForTesting();
  // Checks if all RDMA managed FALCON resources were released. Used to ensure
  // that releasing logic of resources works correctly.
  bool CheckIfAllRdmaManagedFalconResourcesReleasedForTesting();

 private:
  const int id_;
  std::string host_id_;
  const Ipv6Address ip_;
  std::unique_ptr<FakeRdma> rdma_;
  std::unique_ptr<FalconModel> falcon_;
  std::unique_ptr<TrafficShaperModel> traffic_shaper_;
  FalconResourceCredits initial_falcon_resources_;
  FalconCredit initial_rdma_managed_falcon_resources_;
};

// A fake network that can delay, drop, and deliver packets from FalconHost.
class FakeNetwork {
 public:
  explicit FakeNetwork(SimpleEnvironment* env) : env_(env) {}
  // Drops the first packet in packet_buffer_. Returns the information of the
  // dropped packet.
  std::string Drop();
  // Drops packet with rsn of connection with dest_cid in packet_buffer_ .
  // Returns the information of the dropped packet.
  std::string DropByRsn(uint32_t dest_cid, uint32_t rsn);
  // Drops a range of packets in packet_buffer_ index range [start, end).
  // Returns the number of packets dropped.
  int DropMultipleWithIndexRange(uint32_t start, uint32_t end);
  // Delivers a packet in packet_buffer_ after a certain delay. Returns false if
  // there has no packet to deliver.
  bool Deliver(absl::Duration delay);
  // Delivers all packets in the network to the appropriate host.
  void DeliverAll(absl::Duration delay);
  // Injects the packet sent out from host to the network.
  void InjectPacketToNetwork(std::unique_ptr<Packet> packet);
  // Connects to the host.
  void ConnectFalconHost(FalconHost* host);
  // Returns the number of packets held by the network.
  int HeldPacketCount() { return packet_buffer_.size(); }
  // Returns a reference to the first packet in the network.
  const Packet* Peek() const { return packet_buffer_.front().get(); }
  // Returns a reference to the packet at given index in the network.
  const Packet* PeekAt(int index) const {
    return packet_buffer_.at(index).get();
  }
  // Returns the environment elapsed time.
  absl::Duration ElapsedTime() const { return env_->ElapsedTime(); }

  uint64_t host1_counter_rx_ = 0;
  uint64_t host1_counter_tx_ = 0;

  uint64_t host2_counter_rx_ = 0;
  uint64_t host2_counter_tx_ = 0;

 private:
  NetworkPacketBuffer packet_buffer_;
  SimpleEnvironment* const env_;
  FalconHost* host1_;
  FalconHost* host2_;
};

// The fake packet builder connects FalconHost to fake network.
class FakePacketBuilder : public PacketBuilderInterface {
 public:
  void EnqueuePacket(std::unique_ptr<Packet> packet) override {
    packet->timestamps.sent_timestamp = network_->ElapsedTime();
    network_->InjectPacketToNetwork(std::move(packet));
  }
  void EncapsulateAndSendTransportPacket(uint16_t priority) override {}
  void ExtractAndHandleTransportPacket(
      omnetpp::cMessage* network_packet) override {}
  void ConnectNetwork(FakeNetwork* network) { network_ = network; }

 private:
  FakeNetwork* network_;
};

//
// Falcon component test fixture. Enables it when FALCON model is ready for
// testing.
//
// Gen2. Currently, we use the default value of 1 flow per connection For the
// tests that are specific to Gen1, move to a separate file.
class FalconComponentTest
    : public ::testing::TestWithParam<testing::tuple<
          FalconConfig::ResourceReservationMode,
          uint64_t /* inter_host_rx_scheduler_tick_ns */, int /* version */>> {
 public:
  // Allows bringing up Falcon hosts with specific configurations on a per-test
  // basis. In case the tests are parameterized on certain Falcon
  // configurations, then the values passed to this function will be overridden
  // by them. degree_of_multipathing (default value is 1) will set the degree of
  // multipathing for the connection between host1 and host2.
  void InitFalconHosts(FalconConfig falcon_config,
                       uint8_t degree_of_multipathing = 1);
  // Create and submit multiple RDMA Write requests and send to the network.
  void SubmitMultipleUnsolicitedWriteRequests(uint32_t start_rsn,
                                              int num_requests);
  // Target Host2 delivers packets to RDMA.
  void TargetFalconDeliverPacketsToRdma(
      int num_packets, absl::Duration inter_host_rx_scheduler_tick);
  // Process of delivering single unsolicited push packet to host2, host2 RDMA
  // receiving the packet, sending ACK back and host1 RDMA getting completion of
  // the packet with the expected RSN.
  void DeliverPushUnsolicitedDataPacketAndCheckCompletionReceival(uint32_t rsn);

 protected:
  std::unique_ptr<SimpleEnvironment> env_;
  std::unique_ptr<FakeNetwork> network_;
  std::unique_ptr<FalconHost> host1_;
  std::unique_ptr<FalconHost> host2_;

  // Returns the falcon version from the test parameters.
  int GetFalconVersion();

 private:
  FakePacketBuilder packet_builder_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_COMPONENT_TEST_INFRASTRUCTURE_H_
