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

#ifndef ISEKAI_COMMON_MODEL_INTERFACES_H_
#define ISEKAI_COMMON_MODEL_INTERFACES_H_

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon_histograms.h"
#include "isekai/host/falcon/falcon_resource_credits.h"
#include "isekai/host/rdma/rdma_latency_histograms.pb.h"
#include "omnetpp/cmessage.h"
#include "omnetpp/csimplemodule.h"

namespace isekai {

// Provides interfaces for RNIC Models to collect statistics.
class StatisticCollectionInterface {
 public:
  virtual ~StatisticCollectionInterface() {}
  // Updates the statistics with respect to the occurring events.
  virtual absl::Status UpdateStatistic(
      std::string_view stats_output_name, double value,
      StatisticsCollectionConfig_StatisticsType output_type) = 0;
  // Gets the stats collection config.
  virtual StatisticsCollectionConfig& GetConfig() = 0;
};

constexpr double kGiga = 1e9;

// A unique identifier for an RDMA QueuePair.
using QpId = uint32_t;

// Completion callback type, which is specified with an RDMA op and is invoked
// when the op finishes, along with error syndrome if any.
using CompletionCallback = std::function<void(Packet::Syndrome syndrome)>;

// ULP-managed credit for FALCON resource pools.
struct FalconCredit {
  // Counters for each of the six resource pools in PCR-26 interface.
  uint32_t request_tx_packet = 0;
  uint32_t request_tx_buffer = 0;
  uint32_t request_rx_packet = 0;
  uint32_t request_rx_buffer = 0;
  uint32_t response_tx_packet = 0;
  uint32_t response_tx_buffer = 0;

  bool IsInitialized() const {
    return (request_tx_packet != 0 || request_tx_buffer != 0 ||
            request_rx_packet != 0 || request_rx_buffer != 0 ||
            response_tx_packet != 0 || response_tx_buffer != 0);
  }

  bool operator==(const FalconCredit& rhs) const {
    return request_tx_packet == rhs.request_tx_packet &&
           request_tx_buffer == rhs.request_tx_buffer &&
           request_rx_packet == rhs.request_rx_packet &&
           request_rx_buffer == rhs.request_rx_buffer &&
           response_tx_packet == rhs.response_tx_packet &&
           response_tx_buffer == rhs.response_tx_buffer;
  }

  bool operator<(const FalconCredit& rhs) const {
    return request_tx_packet < rhs.request_tx_packet &&
           request_tx_buffer < rhs.request_tx_buffer &&
           request_rx_packet < rhs.request_rx_packet &&
           request_rx_buffer < rhs.request_rx_buffer &&
           response_tx_packet < rhs.response_tx_packet &&
           response_tx_buffer < rhs.response_tx_buffer;
  }
  bool operator<=(const FalconCredit& rhs) const {
    return request_tx_packet <= rhs.request_tx_packet &&
           request_tx_buffer <= rhs.request_tx_buffer &&
           request_rx_packet <= rhs.request_rx_packet &&
           request_rx_buffer <= rhs.request_rx_buffer &&
           response_tx_packet <= rhs.response_tx_packet &&
           response_tx_buffer <= rhs.response_tx_buffer;
  }
  FalconCredit& operator+=(const FalconCredit& rhs) {
    request_tx_packet += rhs.request_tx_packet;
    request_tx_buffer += rhs.request_tx_buffer;
    request_rx_packet += rhs.request_rx_packet;
    request_rx_buffer += rhs.request_rx_buffer;
    response_tx_packet += rhs.response_tx_packet;
    response_tx_buffer += rhs.response_tx_buffer;
    return *this;
  }
  FalconCredit& operator-=(const FalconCredit& rhs) {
    request_tx_packet -= rhs.request_tx_packet;
    request_tx_buffer -= rhs.request_tx_buffer;
    request_rx_packet -= rhs.request_rx_packet;
    request_rx_buffer -= rhs.request_rx_buffer;
    response_tx_packet -= rhs.response_tx_packet;
    response_tx_buffer -= rhs.response_tx_buffer;
    return *this;
  }
  FalconCredit& operator*=(double x) {
    request_tx_packet = std::round(request_tx_packet * x);
    request_tx_buffer = std::round(request_tx_buffer * x);
    request_rx_packet = std::round(request_rx_packet * x);
    request_rx_buffer = std::round(request_rx_buffer * x);
    response_tx_packet = std::round(response_tx_packet * x);
    response_tx_buffer = std::round(response_tx_buffer * x);
    return *this;
  }
  FalconCredit& operator=(const FalconResourceCredits& rhs) {
    request_tx_packet = rhs.tx_packet_credits.ulp_requests;
    request_tx_buffer = rhs.tx_buffer_credits.ulp_requests;
    request_rx_packet = rhs.rx_packet_credits.ulp_requests;
    request_rx_buffer = rhs.rx_buffer_credits.ulp_requests;
    response_tx_packet = rhs.tx_packet_credits.ulp_data;
    response_tx_buffer = rhs.tx_buffer_credits.ulp_data;
    return *this;
  }
  std::string ToString() const {
    std::ostringstream stream;
    stream << "\n request_tx_packet = " << request_tx_packet
           << "\n request_tx_buffer = " << request_tx_buffer
           << "\n request_rx_packet = " << request_rx_packet
           << "\n request_rx_buffer = " << request_rx_buffer
           << "\n response_tx_packet = " << response_tx_packet
           << "\n response_tx_buffer = " << response_tx_buffer;
    return stream.str();
  }
};

enum class RdmaConnectedMode {
  kOrderedRc,  // Represents both strictly and weakly ordered RC.
  kUnorderedRc,
};

enum class QpType { kRC = 5, kUD = 6 };

// Options supplied when creating a QueuePair.
struct QpOptions {
  //
  // fields (such as connection mode, etc) into this QP options struct.
  // Source FALCON channel connection id.
  uint32_t scid = 0;
  // Initial credit for FALCON resource pools. Defaults to 0 for all credit
  // types, letting the RDMA module initialize it to a configured value.
  FalconCredit initial_credit;
  // Destination IP address.
  std::string dst_ip = "::1";

  // The ACK is trigger via two ways: coalescing threshold or timeout, which
  // ever is met first. If the number of coalesced PSN to acknowledge reaches
  // ack_coalescing_threshold, or the timeout expires, and ACK is triggered.
  uint32_t ack_coalescing_threshold = 1;
  absl::Duration ack_coalesing_timeout = absl::ZeroDuration();
  absl::Duration retx_timeout = absl::ZeroDuration();
};

// Options supplied when creating a FalconConnection.
struct FalconConnectionOptions {
  // These two vectors are used to store the forward and reverse static
  // routing port lists for every path.
  // The index of each vector element corresponds to the path ID. Each list
  // specifies the router ports that packets on this path should traverse
  // sequentially, either in the forward or reverse direction. The number of
  // elements in the outer vector must be equal to the degree of multipathing.
  // The number of elements in the nested vector must be equal to the number
  // of routers experienced.
  // This is used only for specific simulation tests that hope to enable
  // static routing.
  std::optional<std::vector<std::vector<uint32_t>>> static_routing_port_lists;
  virtual ~FalconConnectionOptions() {}
};

// Options supplied when creating a multipath FalconConnection.
struct FalconMultipathConnectionOptions : FalconConnectionOptions {
  FalconMultipathConnectionOptions() : FalconConnectionOptions() {}
  FalconMultipathConnectionOptions(uint32_t degree_of_multipathing)
      : FalconConnectionOptions(),
        degree_of_multipathing(degree_of_multipathing) {}
  uint32_t degree_of_multipathing = 1;
};

// Opaque cookie that flows between Falcon and ULP.
struct OpaqueCookie {
  OpaqueCookie() {}
  virtual ~OpaqueCookie() {}
};

// Opaque cookie that flows between Falcon and ULP in Gen2.
struct Gen2OpaqueCookie : OpaqueCookie {
  uint8_t flow_id = 0;
  Gen2OpaqueCookie(uint8_t flow_id) : OpaqueCookie(), flow_id(flow_id) {}
  Gen2OpaqueCookie() {}
};

// An RDMA operation, corresponding to an IB "verb".
enum class RdmaOpcode {
  kSend,
  kRecv,
  kRead,
  kWrite,
};

// Represents the type of per-qp backpressure from Falcon to RDMA.
enum class BackpressureType {
  kXoff,
  kDynamicCredit,
};

// Represents per-qp backpressure data from Falcon to RDMA that decides how the
// QP is throttled and by how much.
struct UlpXoff {
  bool request;
  bool response;
  UlpXoff() : request(false), response(false) {}
};

union BackpressureData {
  UlpXoff xoff;
  FalconCredit credit_limit;
  BackpressureData() : xoff() {}
};

// This interface defines the API between the application (traffic generator) to
// and RDMA block.
class RdmaBaseInterface {
 public:
  virtual ~RdmaBaseInterface() {}
  virtual const RdmaConfig& GetConfig() = 0;
  // From Application.
  // Creates and connects an RC QueuePair.
  virtual void CreateRcQp(QpId local_qp_id, QpId remote_qp_id,
                          QpOptions& options, RdmaConnectedMode rc_mode) = 0;
  // Schedules an RDMA op on the given QueuePair.
  virtual void PerformOp(QpId qp_id, RdmaOpcode opcode,
                         std::vector<uint32_t> sgl, bool is_inline,
                         CompletionCallback completion_callback,
                         QpId dest_qp_id) = 0;
  // Associates (src_qp_id, dst_qp_id) with scid.
  virtual void Associate(QpId src_qp_id, QpId dst_qp_id, uint32_t scid) = 0;
  // Returns all RDMA latency statistics in the given proto.
  virtual void DumpLatencyHistogramsToProto(
      RdmaLatencyHistograms* histograms) = 0;
};

// This interface defines the API between an RDMA block and a FALCON block.
class RdmaFalconInterface {
 public:
  virtual ~RdmaFalconInterface() {}
  // From FALCON.
  // Transfers an RX transaction from FALCON to RDMA.
  virtual void HandleRxTransaction(std::unique_ptr<Packet> packet,
                                   std::unique_ptr<OpaqueCookie> cookie) = 0;
  // Indicates the outcome of a transaction at the initiator. FALCON passes this
  // information to RDMA via this interface for completion generation.
  virtual void HandleCompletion(QpId qp_id, uint32_t rsn,
                                Packet::Syndrome syndrome,
                                uint8_t destination_bifurcation_id) = 0;
  // Returns credit from FALCON.
  virtual void ReturnFalconCredit(QpId qp_id,
                                  const FalconCredit& ulp_credit) = 0;
  // Sets request and global xoff values.
  // If request_xoff = true, RDMA should stop sending all requests (but not
  // responses) to Falcon until request_xoff = false again.
  // If global_xoff = true, RDMA should stop all requests AND responses. This is
  // equivalent to valid/ready bit being set to 0 in hardware.
  virtual void SetXoff(bool request_xoff, bool global_xoff) = 0;

  // Backpressure the QP using different scheme.
  virtual void BackpressureQP(QpId qp_id, BackpressureType type,
                              BackpressureData data) = 0;
};

enum class OrderingMode { kOrdered = 0, kUnordered = 1 };

// Encapsulates the FALCON packet and sends it out to the network.
class PacketBuilderInterface {
 public:
  virtual ~PacketBuilderInterface() {}

  // Traffic shaper calls this function to push a packet to the TX queue of the
  // packet builder.
  virtual void EnqueuePacket(std::unique_ptr<Packet> packet) = 0;
  // Encapsulates a transport packet from TX queue and sends it out.
  virtual void EncapsulateAndSendTransportPacket(uint16_t priority) = 0;
  // Extracts transport packet from a received packet from network.
  virtual void ExtractAndHandleTransportPacket(
      omnetpp::cMessage* network_packet) = 0;
};

class TrafficShaperInterface {
 public:
  virtual ~TrafficShaperInterface() {}

  // Connects packet builder.
  virtual void ConnectPacketBuilder(PacketBuilderInterface* packet_builder) = 0;
  // From FALCON.
  virtual void TransferTxPacket(std::unique_ptr<Packet> packet) = 0;
};

class ConnectionState;

class FalconInterface {
 public:
  virtual ~FalconInterface() {}

  // Get the version of Falcon.
  virtual int GetVersion() const = 0;
  // Transfers a TX transaction from ULP to Falcon.
  virtual void InitiateTransaction(std::unique_ptr<Packet> packet) = 0;
  // Transfers an RX packet from network (Packet Router) to Falcon.
  virtual void TransferRxPacket(std::unique_ptr<Packet> packet) = 0;
  // Signals an ack/nack from ULP to Falcon for a Falcon->ULP transaction.
  virtual void AckTransaction(uint32_t scid, uint32_t rsn,
                              Packet::Syndrome ack_code,
                              absl::Duration rnr_timeout,
                              std::unique_ptr<OpaqueCookie> cookie) = 0;
  // Sets Falcon QueuePair parameters.
  virtual uint32_t SetupNewQp(uint32_t scid, QpId qp_id, QpType qp_type,
                              OrderingMode ordering_mode) = 0;
  // Connects scid to the (dcid, dst_ip_address).
  virtual absl::Status EstablishConnection(
      uint32_t scid, uint32_t dcid, uint8_t source_bifurcation_id,
      uint8_t destination_bifurcation_id, absl::string_view dst_ip_address,
      OrderingMode ordering_mode,
      const FalconConnectionOptions& connection_options) = 0;
  virtual const FalconConfig* get_config() const = 0;
  virtual Environment* get_environment() const = 0;
  virtual std::string_view get_host_id() const = 0;
  virtual StatisticCollectionInterface* get_stats_collector() const = 0;
  virtual FalconHistogramCollector* get_histogram_collector() const = 0;
  // Packet builder calls this function to trigger Xoff in Falcon.
  virtual void SetXoffByPacketBuilder(bool xoff) = 0;
  virtual bool CanSendPacket() const = 0;
  // RDMA calls this function to trigger Xoff in Falcon for the bifurcated host.
  virtual void SetXoffByRdma(uint8_t bifurcation_id, bool xoff) = 0;
  // Update rx bytes.
  virtual void UpdateRxBytes(std::unique_ptr<Packet> packet,
                             uint32_t pkt_size_bytes) = 0;
  // Update tx bytes.
  virtual void UpdateTxBytes(std::unique_ptr<Packet> packet,
                             uint32_t pkt_size_bytes) = 0;
};

// Provides the interface for getting the outband connection metadata, including
// QP information, network information (e.g., mapping from host id to host ip),
// etc. Those metadata should be in a centralized control mode, otherwise it
// would be duplicated by storing in each node in the network.
class ConnectionManagerInterface {
 public:
  virtual ~ConnectionManagerInterface() {}

  // Creates a connection between host_id1 and host_id2, returns local_qp_id,
  // remote_qp_id, local_cid for the connection.
  virtual absl::StatusOr<std::tuple<QpId, QpId>> CreateConnection(
      absl::string_view host_id1, absl::string_view host_id2,
      uint8_t source_bifurcation_id, uint8_t destination_bifurcation_id,
      QpOptions& qp_options, FalconConnectionOptions& connection_options,
      RdmaConnectedMode rc_mode) = 0;
  // Returns a bidirectional connection from host_id1 to host_id2. The
  // connection is created, or the reverse of a previously created bidirectional
  // connection.
  virtual absl::StatusOr<std::tuple<QpId, QpId>>
  GetOrCreateBidirectionalConnection(
      absl::string_view host_id1, absl::string_view host_id2,
      uint8_t source_bifurcation_id, uint8_t destination_bifurcation_id,
      QpOptions& qp_options, FalconConnectionOptions& connection_options,
      RdmaConnectedMode rc_mode) = 0;
  // Create a unidirectional connection from host_id1 to host_id2.
  virtual absl::StatusOr<std::tuple<QpId, QpId>> CreateUnidirectionalConnection(
      absl::string_view host_id1, absl::string_view host_id2,
      uint8_t source_bifurcation_id, uint8_t destination_bifurcation_id,
      QpOptions& qp_options, FalconConnectionOptions& connection_options,
      RdmaConnectedMode rc_mode) = 0;
  // Registers RDMA module. It's called by RdmaBaseModel::
  virtual void RegisterRdma(absl::string_view host_id,
                            RdmaBaseInterface* rdma) = 0;
  // Registers FALCON module.
  virtual void RegisterFalcon(absl::string_view host_id,
                              FalconInterface* falcon) = 0;
  // Populates the host information from simulation_network.
  virtual void PopulateHostInfo(const NetworkConfig& simulation_network) = 0;
  // Get the IP address of a host.
  virtual absl::StatusOr<std::string> GetIpAddress(
      absl::string_view host_id) = 0;
};

class TrafficGeneratorInterface {
 public:
  virtual ~TrafficGeneratorInterface() {}

  virtual void GenerateRdmaOp() = 0;
};

// This interface defines the OmnestHost API used in ASTRA-sim traffic
// generation to avoid circular dependency. ASTRA-sim generates cross-module
// schedule events and needs to call cross-module schedule functions from the
// OmnestHost module.
class IsekaiHostInterface : public omnetpp::cSimpleModule {};

}  // namespace isekai

#endif  // ISEKAI_COMMON_MODEL_INTERFACES_H_
