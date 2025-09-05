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

#ifndef ISEKAI_HOST_FALCON_FALCON_TESTING_HELPERS_H_
#define ISEKAI_HOST_FALCON_FALCON_TESTING_HELPERS_H_

#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/host/falcon/event_response_format_adapter.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_histograms.h"
#include "isekai/host/falcon/gen1/ack_coalescing_engine.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/rate_update_engine.h"
#include "isekai/host/falcon/gen1/rate_update_engine_adapter.h"

namespace isekai {

//
class FalconTestingHelpers {
 public:
  class FakeTrafficShaper : public TrafficShaperInterface {
   public:
    void ConnectPacketBuilder(PacketBuilderInterface* packet_builder) override {
    }
    void TransferTxPacket(std::unique_ptr<Packet> packet) override {
      packet_list.push_back(*packet);
    }
    std::vector<Packet> packet_list;
  };

  class FakeFalconModel : public FalconModelInterface {
   public:
    const std::string id_ = "falcon-host";
    void ReorderCallback(uint32_t cid, uint32_t rsn,
                         falcon::PacketType type) override {
      rsn_order_[cid].push_back(rsn);
    }
    // Maps between CIDs and their processed RSNs in order.
    std::map<uint32_t /*cid*/, std::vector<uint32_t> /*rsn*/> rsn_order_;
    SimpleEnvironment& env_;
    FalconConfig config_;

    FakeFalconModel(FalconConfig& config, SimpleEnvironment& env)
        : env_(env), config_(config) {}

    // Unused virtual functions.
    int GetVersion() const override { return 1; }
    void InitiateTransaction(std::unique_ptr<Packet> packet) override {}
    void TransferRxPacket(std::unique_ptr<Packet> packet) override {}
    void AckTransaction(uint32_t scid, uint32_t rsn, Packet::Syndrome ack_code,
                        absl::Duration rnr_timeout,
                        std::unique_ptr<OpaqueCookie> cookie) override {}
    uint32_t SetupNewQp(uint32_t scid, QpId qp_id, QpType qp_type,
                        OrderingMode ordering_mode) override {
      return 0;
    }
    absl::Status EstablishConnection(
        uint32_t scid, uint32_t dcid, uint8_t source_bifurcation_id,
        uint8_t destination_bifurcation_id, std::string_view dst_ip_address,
        OrderingMode ordering_mode,
        const FalconConnectionOptions& connection_options) override {
      return absl::OkStatus();
    }
    const FalconConfig* get_config() const override { return &config_; };
    Environment* get_environment() const override { return &env_; };
    std::string_view get_host_id() const { return "FakeFalcon"; }
    StatisticCollectionInterface* get_stats_collector() const override {
      return nullptr;
    };
    FalconHistogramCollector* get_histogram_collector() const override {
      return nullptr;
    };
    // Packet builder calls this function to trigger Xoff in Falcon.
    void SetXoffByPacketBuilder(bool xoff) override {}
    bool CanSendPacket() const override { return true; }
    // RDMA calls this function to trigger Xoff in Falcon for the bifurcated
    // host.
    void SetXoffByRdma(uint8_t bifurcation_id, bool xoff) override {}
    // Update rx bytes.
    void UpdateRxBytes(std::unique_ptr<Packet> packet,
                       uint32_t pkt_size_bytes) override {}
    // Update tx bytes.
    void UpdateTxBytes(std::unique_ptr<Packet> packet,
                       uint32_t pkt_size_bytes) override {}

    // Getter for a pointer to the RDMA model.
    RdmaFalconInterface* get_rdma_model() const override { return nullptr; }
    // Getter for a pointer to the traffic shaper.
    TrafficShaperInterface* get_traffic_shaper() const override {
      return nullptr;
    }
    // Getters to Falcon components used by othe internal Falcon components to
    // communicate with each other.
    ConnectionStateManager* get_state_manager() const override {
      return nullptr;
    }
    ResourceManager* get_resource_manager() const override { return nullptr; }
    InterHostRxScheduler* get_inter_host_rx_scheduler() const override {
      return nullptr;
    }
    Scheduler* get_connection_scheduler() const override { return nullptr; }
    Scheduler* get_retransmission_scheduler() const override { return nullptr; }
    Scheduler* get_ack_nack_scheduler() const override { return nullptr; }
    Arbiter* get_arbiter() const override { return nullptr; }
    AdmissionControlManager* get_admission_control_manager() const override {
      return nullptr;
    }
    PacketReliabilityManager* get_packet_reliability_manager() const override {
      return nullptr;
    }
    RateUpdateEngine* get_rate_update_engine() const override {
      return nullptr;
    }
    BufferReorderEngine* get_buffer_reorder_engine() const override {
      return nullptr;
    }
    AckCoalescingEngineInterface* get_ack_coalescing_engine() const override {
      return nullptr;
    }
    StatsManager* get_stats_manager() const override { return nullptr; }
    PacketMetadataTransformer* get_packet_metadata_transformer()
        const override {
      return nullptr;
    }
  };

  template <typename EventT, typename ResponseT>
  class FakeRueAdapter : public RueAdapterInterface {
    static constexpr double kFalconUnitTimeUs = 0.131072;
    const uint32_t kDefaultRetransmissionTimeout =
        std::round(1000 / kFalconUnitTimeUs);  // ~1ms.

   public:
    void Set(std::unique_ptr<EventResponseFormatAdapter<EventT, ResponseT>>
                 format_adapter);
    void ProcessNextEvent(uint32_t now) override;
    int GetNumEvents() const override;
    int GetNumResponses() const;
    void EnqueueAck(const Packet* packet,
                    const CongestionControlMetadata& ccmeta,
                    const RueKey* rue_key, uint32_t num_packets_acked,
                    bool eack, bool eack_drop) override;
    void EnqueueNack(const RueKey* rue_key, const Packet* packet,
                     const CongestionControlMetadata& ccmeta,
                     uint32_t num_packets_acked) override;
    void EnqueueTimeoutRetransmit(const RueKey* rue_key, const Packet* packet,
                                  const CongestionControlMetadata& ccmeta,
                                  uint8_t retransmit_count) override;
    ResponseMetadata DequeueResponse(
        std::function<ConnectionState*(uint32_t connection_id)>
            connection_state_lookup) override;
    void InitializeMetadata(CongestionControlMetadata& metadata) const override;
    EventT FrontEvent() const;
    ResponseT FrontResponse() const;

    const FalconModelInterface* falcon_;

   private:
    std::queue<std::unique_ptr<EventT>> event_queue_;
    std::queue<std::unique_ptr<ResponseT>> response_queue_;
    std::unique_ptr<EventResponseFormatAdapter<EventT, ResponseT>>
        format_adapter_;
  };

  class MockTrafficShaper : public TrafficShaperInterface {
   public:
    MOCK_METHOD(void, ConnectPacketBuilder,
                (PacketBuilderInterface * packet_builder), (override));
    MOCK_METHOD(void, TransferTxPacket, (std::unique_ptr<Packet> packet),
                (override));
  };

  class MockRdma : public RdmaFalconInterface {
   public:
    MOCK_METHOD(void, HandleRxTransaction,
                (std::unique_ptr<Packet> packet,
                 std::unique_ptr<OpaqueCookie> cookie),
                (override));
    MOCK_METHOD(void, HandleCompletion,
                (QpId qp_id, uint32_t rsn, Packet::Syndrome syndrome,
                 uint8_t destination_bifurcation_id),
                (override));
    MOCK_METHOD(void, ReturnFalconCredit,
                (QpId qp_id, const FalconCredit& ulp_credit), (override));
    MOCK_METHOD(void, SetXoff, (bool request_xoff, bool global_xoff),
                (override));
    MOCK_METHOD(void, BackpressureQP,
                (QpId qp_id, BackpressureType type,
                 const BackpressureData data),
                (override));
  };

  // A class that provides common utility functions to set up and run Falcon
  // unit tests as well as provide helper functions that can handle the
  // different versions of Falcon. Currently, it only supports Gen1 and Gen2
  // Falcon.
  class FalconTestSetup {
   protected:
    virtual ~FalconTestSetup() = default;

    // Initializes the correct version of FalconModel from FalconConfig, and
    // connects it to a MockTrafficShaper and MockRdma.
    virtual void InitFalcon(const FalconConfig& config);

    // Creates the right version of OpaqueCookie. flow_id argument is
    // only used for Gen2.
    std::unique_ptr<OpaqueCookie> CreateOpaqueCookie(uint32_t scid,
                                                     uint32_t flow_id = 0);

    // Creates the right version of AckCoalescingKey. flow_label argument is
    // only used for Gen2.
    std::unique_ptr<AckCoalescingKey> CreateAckCoalescingKey(
        uint32_t scid, uint32_t flow_label = 0);

    // Gets the AckCoalescingEntry corresponding to the AckCoalescingKey, based
    // on the version of AckCoalescingEngine.
    absl::StatusOr<const AckCoalescingEntry*> GetAckCoalescingEntry(
        const AckCoalescingKey* ack_coalescing_key);

    // Handles adding a packet to the list of outstanding packets by modifying
    // the TX window's outstanding_packet_contexts and outstanding_packets. For
    // creating a Gen2OutstandingPacketContext, the arguments scid and
    // flow_label are needed to calculate the flow_id.
    void AddOutstandingPacket(ConnectionState* connection_state, uint32_t psn,
                              uint32_t rsn, falcon::PacketType packet_type,
                              uint32_t scid = 1, uint32_t flow_label = 0);

    SimpleEnvironment env_;
    std::unique_ptr<FalconModel> falcon_;
    testing::NiceMock<FalconTestingHelpers::MockTrafficShaper> shaper_;
    testing::NiceMock<FalconTestingHelpers::MockRdma> rdma_;
    ConnectionStateManager* connection_state_manager_;
    PacketReliabilityManager* reliability_manager_;
    AckCoalescingEngineInterface* ack_coalescing_engine_;
    AdmissionControlManager* admission_control_manager_;
    BufferReorderEngine* reorder_engine_;
    Scheduler* connection_scheduler_;
    StatsManager* stats_manager_;
    ResourceManager* resource_manager_;
  };

  static constexpr uint8_t kSourceBifurcationId = 1;
  static constexpr uint8_t kDestinationBifurcationId = 3;

  // Creates a connection metadata struct with a configurable scid and
  // OrderingMode.
  static std::unique_ptr<ConnectionMetadata> InitializeConnectionMetadata(
      const FalconModel* falcon, uint32_t scid = 1,
      OrderingMode ordering_mode = OrderingMode::kOrdered);

  // Initializes a connection in Falcon with the provided connection metadata.
  static ConnectionState* InitializeConnectionState(
      const FalconModel* falcon, std::unique_ptr<ConnectionMetadata> metadata,
      bool expect_ok = true);

  // Create a received packet context based on falcon generation.
  static std::unique_ptr<ReceivedPacketContext> CreateReceivedPacketContext(
      uint32_t falcon_version);

  // Given a connection state, sets up a transaction in that connection with
  // the provided transaction details.
  static Packet* SetupTransactionWithConnectionState(
      ConnectionState* connection_state, TransactionType transaction_type,
      TransactionLocation transaction_location, TransactionState state,
      falcon::PacketType packet_metadata_type, uint32_t rsn, uint32_t psn,
      uint32_t request_length = 1,
      Packet::Rdma::Opcode rdma_opcode = Packet::Rdma::Opcode::kInvalid);

  // Given a connection state, sets up an incoming transaction in that
  // connection with the provided transaction details.
  static Packet* SetupIncomingTransactionWithConnectionState(
      ConnectionState* connection_state, TransactionType transaction_type,
      TransactionLocation transaction_location, TransactionState state,
      falcon::PacketType packet_metadata_type, uint32_t rsn, uint32_t psn,
      uint32_t request_length = 1,
      Packet::Rdma::Opcode rdma_opcode = Packet::Rdma::Opcode::kInvalid);

  // Helper function to instantiate a transaction which has (scid = 1, rsn =
  // 0, psn = 0) and type as decided by the caller.
  static std::pair<Packet*, ConnectionState*> SetupTransaction(
      const FalconModel* falcon, TransactionType transaction_type,
      TransactionLocation transaction_location, TransactionState state,
      falcon::PacketType packet_metadata_type, uint32_t scid = 1,
      uint32_t rsn = 0, uint32_t psn = 0,
      OrderingMode ordering_mode = OrderingMode::kOrdered);

  // Creates a Packet with the given fields.
  static std::unique_ptr<Packet> CreatePacket(falcon::PacketType type,
                                              uint32_t dest_cid, uint32_t psn,
                                              uint32_t rsn, bool ack_req,
                                              uint32_t flow_label);

  // Creates EACK packet with the given fields
  static std::unique_ptr<Packet> CreateEackPacket(
      uint32_t dest_cid, uint32_t rdbpsn, uint32_t rrbpsn, absl::Duration t4,
      absl::Duration t1, bool data_own = false, bool request_own = false);

  // Sets the degree of multipathing for a given connection metadata.
  static void SetMultipathingDegree(ConnectionMetadata* connection_metadata,
                                    uint8_t degree_of_multipathing);
};

// This is a peer class that is defined to allow access to private functions
// and variables in ProtocolRateUpdateEngine for testing purposes.
template <typename EventT, typename ResponseT>
class ProtocolRateUpdateEngineTestPeer {
 public:
  void Set(ProtocolRateUpdateEngine* rue);
  uint32_t ToFalconTimeUnits(absl::Duration time) const;
  absl::Duration FromFalconTimeUnits(uint32_t time) const;
  uint32_t ToTimingWheelTimeUnits(absl::Duration time) const;
  absl::Duration FromTimingWheelTimeUnits(uint32_t time) const;
  // Increments the num_acked in the connection state for the RueKey that the
  // incoming_packet belongs to.
  void IncrementNumAcked(const Packet* incoming_packet, uint32_t num_acked);
  bool IsEventQueueScheduled() const;
  int GetNumEvents() const;
  int GetNumResponses() const;
  EventT FrontEvent() const;
  ResponseT FrontResponse() const;
  // Creates the right type of RueKey unique_ptr based on the falcon version.
  std::unique_ptr<RueKey> GetRueKey(uint32_t cid, uint8_t flow_id) const;
  bool IsConnectionOutstanding(uint32_t cid, uint8_t flow_id = 0) const;
  uint32_t GetAccumulatedAcks(uint32_t cid, uint8_t flow_id = 0) const;
  absl::Duration GetLastEventTime(uint32_t cid, uint8_t flow_id = 0) const;
  void set_event_queue_thresholds(uint64_t threshold1, uint64_t threshold2,
                                  uint64_t threshold3);
  void set_predicate_1_time_threshold(absl::Duration t);
  void set_predicate_2_packet_count_threshold(uint32_t t);

 private:
  ProtocolRateUpdateEngine* rue_;
  FalconTestingHelpers::FakeRueAdapter<EventT, ResponseT>* rue_adapter_;
};

template <typename EventT, typename ResponseT>
class MockRueAdapter : public RueAdapterInterface {
 public:
  MOCK_METHOD(void, ProcessNextEvent, (uint32_t now), (override));
  MOCK_METHOD(int, GetNumEvents, (), (const, override));
  MOCK_METHOD(void, EnqueueAck,
              (const Packet* packet, const CongestionControlMetadata& ccmeta,
               const RueKey* rue_key, uint32_t num_packets_acked, bool eack,
               bool eack_drop),
              (override));
  MOCK_METHOD(void, EnqueueNack,
              (const RueKey* rue_key, const Packet* packet,
               const CongestionControlMetadata& ccmeta,
               uint32_t num_packets_acked),
              (override));
  MOCK_METHOD(void, EnqueueTimeoutRetransmit,
              (const RueKey* rue_key, const Packet* packet,
               const CongestionControlMetadata& ccmeta,
               uint8_t retransmit_count),
              (override));
  MOCK_METHOD(ResponseMetadata, DequeueResponse,
              (std::function<ConnectionState*(uint32_t connection_id)>
                   connection_state_lookup),
              (override));
  MOCK_METHOD(void, InitializeMetadata, (CongestionControlMetadata & metadata),
              (const, override));
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_TESTING_HELPERS_H_
