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

#include "isekai/host/falcon/falcon_histograms.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "isekai/common/tdigest.h"

namespace isekai {
namespace {
// Lists of all transaction latency types for easier iteration.
constexpr std::array<PushUnsolicitedLatencyTypes, 7> kPushUnsolicitedTypes = {
    {PushUnsolicitedLatencyTypes::kRxUlpDataToTxNetworkData,
     PushUnsolicitedLatencyTypes::kTxNetworkDataToRxNetworkAck,
     PushUnsolicitedLatencyTypes::kRxNetworkAckToTxUlpCompletion,
     PushUnsolicitedLatencyTypes::kRxNetworkDataToTxUlpData,
     PushUnsolicitedLatencyTypes::kTxUlpDataToRxUlpAck,
     PushUnsolicitedLatencyTypes::kTotalLatency}};

constexpr std::array<PushSolicitedLatencyTypes, 12> kPushSolicitedTypes = {
    {PushSolicitedLatencyTypes::kRxUlpDataToTxNetworkRequest,
     PushSolicitedLatencyTypes::kTxNetworkRequestToRxNetworkGrant,
     PushSolicitedLatencyTypes::kRxNetworkGrantToTxNetworkData,
     PushSolicitedLatencyTypes::kTxNetworkDataToRxNetworkAck,
     PushSolicitedLatencyTypes::kRxNetworkAckToTxUlpCompletion,
     PushSolicitedLatencyTypes::kRxNetworkRequestToTxNetworkGrant,
     PushSolicitedLatencyTypes::kTxNetworkGrantToRxNetworkData,
     PushSolicitedLatencyTypes::kRxNetworkDataToTxUlpData,
     PushSolicitedLatencyTypes::kTxUlpDataToRxUlpAck,
     PushSolicitedLatencyTypes::kTxNetworkGrantToTxUlpData,
     PushSolicitedLatencyTypes::kTotalLatency}};

constexpr std::array<PullLatencyTypes, 10> kPullTypes = {
    {PullLatencyTypes::kRxUlpRequestToTxNetworkRequest,
     PullLatencyTypes::kTxNetworkRequestToRxNetworkData,
     PullLatencyTypes::kRxNetworkDataToTxUlpData,
     PullLatencyTypes::kTxNetworkRequestToTxUlpData,
     PullLatencyTypes::kRxNetworkRequestToTxUlpRequest,
     PullLatencyTypes::kTxUlpRequestToRxUlpRequestAck,
     PullLatencyTypes::kRxUlpRequestAckToRxUlpData,
     PullLatencyTypes::kRxUlpDataToTxNetworkData,
     PullLatencyTypes::kTxNetworkDataToRxNetworkAck,
     PullLatencyTypes::kTotalLatency}};

constexpr std::array<SchedulerTypes, 3> kSchedulerTypes = {
    {SchedulerTypes::kConnection, SchedulerTypes::kRetransmission,
     SchedulerTypes::kAckNack}};

constexpr std::array<XoffTypes, 2> kXoffTypes = {
    {XoffTypes::kFalcon, XoffTypes::kRdma}};

// Helper methods to convert various latency types to human friendly strings.
std::string TypeToString(PushUnsolicitedLatencyTypes type) {
  switch (type) {
    case PushUnsolicitedLatencyTypes::kRxUlpDataToTxNetworkData:
      return "Initiator-RxUlpData-to-TxNetworkData";
    case PushUnsolicitedLatencyTypes::kTxNetworkDataToRxNetworkAck:
      return "Initiator-TxNetworkData-to-RxNetworkAck";
    case PushUnsolicitedLatencyTypes::kRxNetworkAckToTxUlpCompletion:
      return "Initiator-RxNetworkAck-to-TxUlpCompletion";
    case PushUnsolicitedLatencyTypes::kRxNetworkDataToTxUlpData:
      return "Target-RxNetworkData-to-TxUlpData";
    case PushUnsolicitedLatencyTypes::kTxUlpDataToRxUlpAck:
      return "Target-TxUlpData-to-RxUlpAck";
    case PushUnsolicitedLatencyTypes::kTotalLatency:
      return "Initiator-Total-Transaction-Latency";
  }
}

std::string TypeToString(PushSolicitedLatencyTypes type) {
  switch (type) {
    case PushSolicitedLatencyTypes::kRxUlpDataToTxNetworkRequest:
      return "Initiator-RxUlpData-to-TxNetworkRequest";
    case PushSolicitedLatencyTypes::kTxNetworkRequestToRxNetworkGrant:
      return "Initiator-TxNetworkRequest-to-RxNetworkGrant";
    case PushSolicitedLatencyTypes::kRxNetworkGrantToTxNetworkData:
      return "Initiator-RxNetworkGrant-to-TxNetworkData";
    case PushSolicitedLatencyTypes::kTxNetworkDataToRxNetworkAck:
      return "Initiator-TxNetworkData-to-RxNetworkAck";
    case PushSolicitedLatencyTypes::kRxNetworkAckToTxUlpCompletion:
      return "Initiator-RxNetworkAck-to-TxUlpCompletion";
    case PushSolicitedLatencyTypes::kRxNetworkRequestToTxNetworkGrant:
      return "Target-RxNetworkRequest-to-TxNetworkGrant";
    case PushSolicitedLatencyTypes::kTxNetworkGrantToRxNetworkData:
      return "Target-TxNetworkGrant-to-RxNetworkData";
    case PushSolicitedLatencyTypes::kRxNetworkDataToTxUlpData:
      return "Target-RxNetworkData-to-TxUlpData";
    case PushSolicitedLatencyTypes::kTxUlpDataToRxUlpAck:
      return "Target-TxUlpData-to-RxUlpAck";
    case PushSolicitedLatencyTypes::kTxNetworkGrantToTxUlpData:
      return "Target-TxNetworkGrant-to-TxUlpData";
    case PushSolicitedLatencyTypes::kTotalLatency:
      return "Initiator-Total-Transaction-Latency";
  }
}

std::string TypeToString(PullLatencyTypes type) {
  switch (type) {
    case PullLatencyTypes::kRxUlpRequestToTxNetworkRequest:
      return "Initiator-RxUlpRequest-to-TxNetworkRequest";
    case PullLatencyTypes::kTxNetworkRequestToRxNetworkData:
      return "Initiator-TxNetworkRequest-to-RxNetworkData";
    case PullLatencyTypes::kRxNetworkDataToTxUlpData:
      return "Initiator-RxNetworkData-to-TxUlpData";
    case PullLatencyTypes::kTxNetworkRequestToTxUlpData:
      return "Initiator-TxNetworkRequest-to-TxUlpData";
    case PullLatencyTypes::kRxNetworkRequestToTxUlpRequest:
      return "Target-RxNetworkRequest-to-TxUlpRequest";
    case PullLatencyTypes::kTxUlpRequestToRxUlpRequestAck:
      return "Target-TxUlpRequest-to-RxUlpRequestAck";
    case PullLatencyTypes::kRxUlpRequestAckToRxUlpData:
      return "Target-RxUlpRequestAck-to-RxUlpData";
    case PullLatencyTypes::kRxUlpDataToTxNetworkData:
      return "Target-RxUlpData-to-TxNetworkData";
    case PullLatencyTypes::kTxNetworkDataToRxNetworkAck:
      return "Target-TxNetworkData-to-RxNetworkAck";
    case PullLatencyTypes::kTotalLatency:
      return "Initiator-Total-Transaction-Latency";
  }
}

std::string TypeToString(SchedulerTypes type) {
  switch (type) {
    case SchedulerTypes::kConnection:
      return "ConnectionScheduler";
    case SchedulerTypes::kRetransmission:
      return "RetransmissionScheduler";
    case SchedulerTypes::kAckNack:
      return "AckNackScheduler";
  }
}

std::string TypeToString(XoffTypes type) {
  switch (type) {
    case XoffTypes::kFalcon:
      return "Falcon-Xoff";
    case XoffTypes::kRdma:
      return "Rdma-Xoff";
  }
}

}  // namespace

FalconHistogramCollector::FalconHistogramCollector() { Reset(); }

void FalconHistogramCollector::Reset() {
  for (const auto& x : kPushUnsolicitedTypes) {
    push_unsolicited_[x] = TDigest::New(kTdigestCompression);
  }
  for (const auto& x : kPushSolicitedTypes) {
    push_solicited_[x] = TDigest::New(kTdigestCompression);
  }
  for (const auto& x : kPullTypes) {
    pull_[x] = TDigest::New(kTdigestCompression);
  }
  for (const auto& x : kSchedulerTypes) {
    scheduler_[x] = TDigest::New(kTdigestCompression);
  }
  for (const auto& x : kXoffTypes) {
    xoff_[x] = TDigest::New(kTdigestCompression);
  }
}

void FalconHistogramCollector::Add(PushUnsolicitedLatencyTypes type,
                                   double value) {
  push_unsolicited_[type]->Add(value);
}
void FalconHistogramCollector::Add(PushSolicitedLatencyTypes type,
                                   double value) {
  push_solicited_[type]->Add(value);
}
void FalconHistogramCollector::Add(PullLatencyTypes type, double value) {
  pull_[type]->Add(value);
}
void FalconHistogramCollector::Add(SchedulerTypes type, double value) {
  scheduler_[type]->Add(value);
}
void FalconHistogramCollector::Add(XoffTypes type, double value) {
  xoff_[type]->Add(value);
}
void FalconHistogramCollector::Add(uint32_t cid,
                                   FalconConnectionCounters& counters) {
  connection_scheduler_.emplace(
      cid, std::make_unique<IntraConnectionSchedulerTdigests>());
  connection_scheduler_[cid]->pull_and_ordered_push_request_queue_packets->Add(
      counters.pull_and_ordered_push_request_queue_packets);
  connection_scheduler_[cid]->unordered_push_request_queue_packets->Add(
      counters.unordered_push_request_queue_packets);
  connection_scheduler_[cid]->pull_data_queue_packets->Add(
      counters.pull_data_queue_packets);
  connection_scheduler_[cid]->push_grant_queue_packets->Add(
      counters.push_grant_queue_packets);
  connection_scheduler_[cid]->push_data_queue_packets->Add(
      counters.push_data_queue_packets);
}
void FalconHistogramCollector::Add(uint32_t cid, falcon::PacketType type,
                                   bool is_ordered_connection,
                                   bool is_tx_eligible_queueing_delay,
                                   double value) {
  connection_scheduler_queueing_delay_.emplace(
      cid, std::make_unique<IntraConnectionSchedulerQueueingDelayTdigests>());
  switch (type) {
    case falcon::PacketType::kPullRequest:
      connection_scheduler_queueing_delay_[cid]
          ->pull_and_ordered_push_request_queueing_delay->Add(value);
      break;
    case falcon::PacketType::kPushRequest:
      if (is_ordered_connection) {
        connection_scheduler_queueing_delay_[cid]
            ->pull_and_ordered_push_request_queueing_delay->Add(value);
      } else {
        connection_scheduler_queueing_delay_[cid]
            ->unordered_push_request_queueing_delay->Add(value);
      }
      break;
    case falcon::PacketType::kPushSolicitedData:
    case falcon::PacketType::kPushUnsolicitedData:
      if (is_tx_eligible_queueing_delay) {
        connection_scheduler_queueing_delay_[cid]
            ->tx_eligible_push_data_queueing_delay->Add(value);
      } else {
        connection_scheduler_queueing_delay_[cid]
            ->push_data_queueing_delay->Add(value);
      }
      break;
    case falcon::PacketType::kPushGrant:
      connection_scheduler_queueing_delay_[cid]->push_grant_queueing_delay->Add(
          value);
      break;
    case falcon::PacketType::kPullData:
      connection_scheduler_queueing_delay_[cid]->pull_data_queueing_delay->Add(
          value);
      break;
    case falcon::PacketType::kInvalid:  // Phantom requests.
      connection_scheduler_queueing_delay_[cid]
          ->pull_and_ordered_push_request_queueing_delay->Add(value);
      break;
    default:
      break;
  }
}

std::string FalconHistogramCollector::TdigestSummary(TDigest* const digest) {
  std::ostringstream stream;
  // clang-format off
  stream << std::setprecision(3)
         << digest->Min() << "   "
         << digest->Max() << "   "
         << digest->Sum() / digest->Count() << "   "
         << digest->Quantile(0.5) << "   "
         << digest->Quantile(0.9) << "   "
         << digest->Quantile(0.99) << std::endl;
  // clang-format on
  return stream.str();
}

std::string FalconHistogramCollector::Summarize() {
  std::ostringstream stream;
  stream << std::endl
         << std::setw(50) << "Push Unsolicited  "
         << "|   min     max     avg     p50     p90     p99\n"
         << std::string(100, '-') << std::endl;
  for (const auto& x : kPushUnsolicitedTypes) {
    TDigest* digest = push_unsolicited_[x].get();
    stream << std::setprecision(3) << std::setw(50) << TypeToString(x) << " | "
           << TdigestSummary(digest);
  }
  stream << std::endl
         << std::setw(50) << "Push Solicited  "
         << "|   min     max     avg     p50     p90     p99\n"
         << std::string(100, '-') << std::endl;
  for (const auto& x : kPushSolicitedTypes) {
    TDigest* digest = push_solicited_[x].get();
    stream << std::setprecision(3) << std::setw(50) << TypeToString(x) << " | "
           << TdigestSummary(digest);
  }
  stream << std::endl
         << std::setw(50) << "Pull  "
         << "|   min     max     avg     p50     p90     p99\n"
         << std::string(100, '-') << std::endl;
  for (const auto& x : kPullTypes) {
    TDigest* digest = pull_[x].get();
    stream << std::setprecision(3) << std::setw(50) << TypeToString(x) << " | "
           << TdigestSummary(digest);
  }
  stream << std::endl
         << std::setw(50) << "Schedulers  "
         << "|   min     max     avg     p50     p90     p99\n"
         << std::string(100, '-') << std::endl;
  for (const auto& x : kSchedulerTypes) {
    TDigest* digest = scheduler_[x].get();
    stream << std::setprecision(3) << std::setw(50) << TypeToString(x) << " | "
           << TdigestSummary(digest);
  }
  stream << std::endl
         << std::setw(50) << "Falcon XoFFs  "
         << "|   min     max     avg     p50     p90     p99\n"
         << std::string(100, '-') << std::endl;
  for (const auto& x : kXoffTypes) {
    TDigest* digest = xoff_[x].get();
    stream << std::setprecision(3) << std::setw(50) << TypeToString(x) << " | "
           << TdigestSummary(digest);
  }
  stream << "\n\n";

  return stream.str();
}

void FalconHistogramCollector::DumpAllHistogramsToProto(
    FalconHistograms* histograms) {
  for (const auto& x : kPushUnsolicitedTypes) {
    TDigest* digest = push_unsolicited_[x].get();
    TransactionLatencyTdigest* histogram = histograms->add_histogram();
    histogram->set_transaction_type("PushUnsolicited");
    histogram->set_transaction_stage(TypeToString(x));
    digest->ToProto(histogram->mutable_latency_tdigest());
  }
  for (const auto& x : kPushSolicitedTypes) {
    TDigest* digest = push_solicited_[x].get();
    TransactionLatencyTdigest* histogram = histograms->add_histogram();
    histogram->set_transaction_type("PushSolicited");
    histogram->set_transaction_stage(TypeToString(x));
    digest->ToProto(histogram->mutable_latency_tdigest());
  }
  for (const auto& x : kPullTypes) {
    TDigest* digest = pull_[x].get();
    TransactionLatencyTdigest* histogram = histograms->add_histogram();
    histogram->set_transaction_type("Pull");
    histogram->set_transaction_stage(TypeToString(x));
    digest->ToProto(histogram->mutable_latency_tdigest());
  }
  for (const auto& x : kSchedulerTypes) {
    TDigest* digest = scheduler_[x].get();
    SchedulerLengthTdigest* histogram = histograms->add_scheduler_histogram();
    histogram->set_scheduler_type(TypeToString(x));
    digest->ToProto(histogram->mutable_length_tdigest());
  }
  for (const auto& x : kXoffTypes) {
    TDigest* digest = xoff_[x].get();
    XoffDurationTdigest* histogram = histograms->add_xoff_histogram();
    histogram->set_xoff_type(TypeToString(x));
    digest->ToProto(histogram->mutable_latency_tdigest());
  }
  for (const auto& x : connection_scheduler_) {
    auto* digests = x.second.get();
    ConnectionSchedulerQueuesTdigest* histogram =
        histograms->add_connection_scheduler_histogram();
    histogram->set_connection_id(absl::StrCat(x.first));
    digests->pull_and_ordered_push_request_queue_packets->ToProto(
        histogram->mutable_pull_and_ordered_push_request_queue_tdigest());
    digests->unordered_push_request_queue_packets->ToProto(
        histogram->mutable_unordered_push_request_queue_tdigest());
    digests->pull_data_queue_packets->ToProto(
        histogram->mutable_pull_data_queue_tdigest());
    digests->push_grant_queue_packets->ToProto(
        histogram->mutable_push_grant_queue_tdigest());
    digests->push_data_queue_packets->ToProto(
        histogram->mutable_push_data_queue_tdigest());
  }
  for (const auto& x : connection_scheduler_queueing_delay_) {
    auto* digests = x.second.get();
    ConnectionSchedulerQueueingDelayTdigest* histogram =
        histograms->add_intra_connection_scheduler_queueing_delay_histogram();
    histogram->set_connection_id(absl::StrCat(x.first));
    digests->pull_and_ordered_push_request_queueing_delay->ToProto(
        histogram->mutable_pull_and_ordered_push_request_queue_tdigest());
    digests->unordered_push_request_queueing_delay->ToProto(
        histogram->mutable_unordered_push_request_queue_tdigest());
    digests->push_data_queueing_delay->ToProto(
        histogram->mutable_push_data_queue_tdigest());
    digests->tx_eligible_push_data_queueing_delay->ToProto(
        histogram->mutable_tx_eligible_push_data_queue_tdigest());
    digests->push_grant_queueing_delay->ToProto(
        histogram->mutable_push_grant_queue_tdigest());
    digests->pull_data_queueing_delay->ToProto(
        histogram->mutable_pull_data_queue_tdigest());
  }
}

}  // namespace isekai
