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

#ifndef ISEKAI_HOST_FALCON_FALCON_HISTOGRAMS_H_
#define ISEKAI_HOST_FALCON_FALCON_HISTOGRAMS_H_

#include <cstdint>
#include <memory>
#include <string>

//
// open_source_default tdigest.
#include "absl/container/flat_hash_map.h"
#include "isekai/common/tdigest.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_counters.h"
#include "isekai/host/falcon/falcon_histograms.pb.h"

namespace isekai {
// Using a default compression of 100, as TDigest test-cases use this value.
static constexpr double kTdigestCompression = 100.0;

// We breakdown FALCON transaction latency between various states, where each
// state corresponds to a FALCON state transition that happens when a packet (or
// Op) is received or sent to and from Ulp or the network. Each state is
// represented as

//   {Rx|Tx}-{Network|Ulp}-{PacketType}

// For e.g., TxNetworkData means that a data packet was transmitted to the
// network by FALCON. Each latency type below captures the time between two
// successive FALCON state transitions.
// We further group the latency statistics based on transaction type, and
// whether it recorded at the initiator or the target, so that it is easy to
// analyze the latency statistics.

enum class PushUnsolicitedLatencyTypes {
  // As Initiator.
  kRxUlpDataToTxNetworkData,     // Delay in push unsolicited FALCON scheduling.
  kTxNetworkDataToRxNetworkAck,  // Network delay of push unsolicited data.
  kRxNetworkAckToTxUlpCompletion,  // Delay in initiator reorder engine.
  // As Target.
  kRxNetworkDataToTxUlpData,  // Delay in target reorder engine.
  kTxUlpDataToRxUlpAck,       // ULP push unsolicited data processing delay.
  // Total latency from transaction start to finish at the initiator.
  kTotalLatency,
};

enum class PushSolicitedLatencyTypes {
  // As Initiator.
  kRxUlpDataToTxNetworkRequest,  // Delay in push request FALCON scheduling.
  kTxNetworkRequestToRxNetworkGrant,  // Push grant network latency.
  kRxNetworkGrantToTxNetworkData,  // Grant reorder and data scheduling delay.
  kTxNetworkDataToRxNetworkAck,    // Solicited data network delay.
  kRxNetworkAckToTxUlpCompletion,  // Delay in completion in reoder engine.
  // As Target.
  kRxNetworkRequestToTxNetworkGrant,  // Request reorder and grant scheduling.
  kTxNetworkGrantToRxNetworkData,     // Push solicited data network latency.
  kRxNetworkDataToTxUlpData,   // Push solicited data reorder engine delay.
  kTxUlpDataToRxUlpAck,        // ULP push solicited data processing delay.
  kTxNetworkGrantToTxUlpData,  // Solicitation window occupancy latency.
  // Total latency from transaction start to finish at the initiator.
  kTotalLatency,
};

enum class PullLatencyTypes {
  // As Initiator.
  kRxUlpRequestToTxNetworkRequest,   // Pull request FALCON scheduling delay.
  kTxNetworkRequestToRxNetworkData,  // Pull data network latency.
  kRxNetworkDataToTxUlpData,         // Pull data reorder engine delay.
  kTxNetworkRequestToTxUlpData,      // Solicitation window occupancy latency.
  // As Target.
  kRxNetworkRequestToTxUlpRequest,  // Pull request reorder engine delay.
  kTxUlpRequestToRxUlpRequestAck,   // Pull request ULP ack delay, non-critical.
  kRxUlpRequestAckToRxUlpData,      // Pull data ULP processing delay.
  kRxUlpDataToTxNetworkData,        // Pull data FALCON scheduling delay.
  kTxNetworkDataToRxNetworkAck,     // Pull data network latency.
  // Total latency from transaction start to finish at the initiator.
  kTotalLatency,
};

enum class SchedulerTypes {
  kConnection,      // Connection scheduler queue.
  kRetransmission,  // Retransmission scheduler queue.
  kAckNack,         // (N)ACK scheduler queue.
};

enum class XoffTypes {
  kFalcon,  // Falcon Xoff (triggered by packet builder).
  kRdma,    // Rdma Xoff (triggered by Falcon).
};

// Per-connection intra connection scheduler queue histograms.
struct IntraConnectionSchedulerTdigests {
  std::unique_ptr<TDigest> pull_and_ordered_push_request_queue_packets =
      TDigest::New(kTdigestCompression);
  std::unique_ptr<TDigest> unordered_push_request_queue_packets =
      TDigest::New(kTdigestCompression);
  std::unique_ptr<TDigest> push_data_queue_packets =
      TDigest::New(kTdigestCompression);
  std::unique_ptr<TDigest> push_grant_queue_packets =
      TDigest::New(kTdigestCompression);
  std::unique_ptr<TDigest> pull_data_queue_packets =
      TDigest::New(kTdigestCompression);
};

struct IntraConnectionSchedulerQueueingDelayTdigests {
  std::unique_ptr<TDigest> pull_and_ordered_push_request_queueing_delay =
      TDigest::New(kTdigestCompression);
  std::unique_ptr<TDigest> unordered_push_request_queueing_delay =
      TDigest::New(kTdigestCompression);
  std::unique_ptr<TDigest> push_data_queueing_delay =
      TDigest::New(kTdigestCompression);
  std::unique_ptr<TDigest> tx_eligible_push_data_queueing_delay =
      TDigest::New(kTdigestCompression);
  std::unique_ptr<TDigest> push_grant_queueing_delay =
      TDigest::New(kTdigestCompression);
  std::unique_ptr<TDigest> pull_data_queueing_delay =
      TDigest::New(kTdigestCompression);
};

class FalconHistogramCollector {
 public:
  FalconHistogramCollector();
  // Clears all stored statistics for all transactions and all stages.
  void Reset();
  // Adds a latency sample to relevant transaction type and transaction stage.
  void Add(PushUnsolicitedLatencyTypes type, double value);
  void Add(PushSolicitedLatencyTypes type, double value);
  void Add(PullLatencyTypes type, double value);
  // Adds a outstanding packet count sample to the relevant scheduler type.
  void Add(SchedulerTypes type, double value);
  // Adds an Xoff duration sample to the relevant Xoff type.
  void Add(XoffTypes type, double value);
  // Adds outstanding packet count in an intra connection scheduler queue to the
  // relevant per-connection histogram.
  void Add(uint32_t cid, FalconConnectionCounters& counters);
  void Add(uint32_t cid, falcon::PacketType type, bool is_ordered_connection,
           bool is_tx_eligible_queueing_delay, double value);
  // Returns the TDigest of the give transaction type and stage.
  TDigest& GetTdigest(PushUnsolicitedLatencyTypes type) {
    return *push_unsolicited_[type];
  }
  TDigest& GetTdigest(PushSolicitedLatencyTypes type) {
    return *push_solicited_[type];
  }
  TDigest& GetTdigest(PullLatencyTypes type) { return *pull_[type]; }
  // Returns the TDigest of the given Xoff type.
  TDigest& GetTdigest(XoffTypes type) { return *xoff_[type]; }
  // Returns a string representation of the given scheduler type.
  TDigest& GetTdigest(SchedulerTypes type) { return *scheduler_[type]; }
  // Returns a string summarizing the (a) latency breakdown for all transaction
  // types and all stages and (b) scheduler lengths in a human readable format.
  std::string Summarize();
  // Fills in the latency histograms and scheduler histograms (TDigest
  // representation) of all transaction types and all stages inside the given
  // protobuf message.
  void DumpAllHistogramsToProto(FalconHistograms* histograms);

 private:
  // Prints a human readable summary (min, max, avg, %tile) for a given tdigest.
  std::string TdigestSummary(TDigest* digest);

  absl::flat_hash_map<PushUnsolicitedLatencyTypes, std::unique_ptr<TDigest>>
      push_unsolicited_;
  absl::flat_hash_map<PushSolicitedLatencyTypes, std::unique_ptr<TDigest>>
      push_solicited_;
  absl::flat_hash_map<PullLatencyTypes, std::unique_ptr<TDigest>> pull_;
  absl::flat_hash_map<SchedulerTypes, std::unique_ptr<TDigest>> scheduler_;
  absl::flat_hash_map<XoffTypes, std::unique_ptr<TDigest>> xoff_;
  absl::flat_hash_map<uint32_t,
                      std::unique_ptr<IntraConnectionSchedulerTdigests>>
      connection_scheduler_;
  absl::flat_hash_map<
      uint32_t, std::unique_ptr<IntraConnectionSchedulerQueueingDelayTdigests>>
      connection_scheduler_queueing_delay_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_HISTOGRAMS_H_
