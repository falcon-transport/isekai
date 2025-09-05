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

#include "isekai/host/falcon/falcon_connection_state.h"

#include <cstdint>
#include <memory>
#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/time/time.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/connection_state_utils.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_histograms.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"

namespace isekai {

// Creates/updates transaction as Falcon receives RDMA op. It also updates the
// transaction state and creates the corresponding packet metadata. Returns the
// (RSN, Packet Type).
UlpTransactionInfo ConnectionState::CreateOrUpdateTransactionFromUlp(
    std::unique_ptr<Packet> packet, const int solicited_write_threshold,
    OrderingMode ordering_mode, FalconInterface* falcon) {
  uint32_t rsn;
  TransactionType transaction_type;
  TransactionLocation location;
  falcon::PacketType packet_type;
  PacketDirection direction;
  uint32_t data_length = packet->rdma.data_length;
  uint32_t sgl_length = packet->metadata.sgl_length;
  switch (packet->rdma.opcode) {
    case Packet::Rdma::Opcode::kReadRequest:
      packet_type = falcon::PacketType::kPullRequest;
      rsn = tx_transaction_metadata.next_request_sequence_number++;
      transaction_type = TransactionType::kPull;
      location = TransactionLocation::kInitiator;
      direction = PacketDirection::kOutgoing;
      break;
    case Packet::Rdma::Opcode::kReadResponseOnly:
      packet_type = falcon::PacketType::kPullData;
      rsn = packet->rdma.rsn;
      transaction_type = TransactionType::kPull;
      location = TransactionLocation::kTarget;
      direction = PacketDirection::kOutgoing;
      break;
    case Packet::Rdma::Opcode::kWriteOnly:
    case Packet::Rdma::Opcode::kSendOnly:
      rsn = tx_transaction_metadata.next_request_sequence_number++;
      if (packet->rdma.request_length >= solicited_write_threshold) {
        transaction_type = TransactionType::kPushSolicited;
        packet_type = falcon::PacketType::kPushSolicitedData;
      } else {
        transaction_type = TransactionType::kPushUnsolicited;
        packet_type = falcon::PacketType::kPushUnsolicitedData;
      }
      location = TransactionLocation::kInitiator;
      direction = PacketDirection::kOutgoing;
      break;
    case Packet::Rdma::Opcode::kAck:
      LOG(FATAL) << "RDMA does not generate an explicit ACK packet in "
                    "RDMA-Falcon mode.";
    case Packet::Rdma::Opcode::kInvalid:
      LOG(FATAL)
          << "Received a packet from RDMA without its opcode initialized.";
  }

  TransactionKey transaction_key(rsn, location);
  if (location == TransactionLocation::kInitiator) {
    transactions[transaction_key] =
        CreateTransactionMetadata(version, rsn, transaction_type, location);
    transactions.at(transaction_key)->request_length =
        packet->rdma.request_length;
  }

  // If it is push unsolicited data in ordered connection, we need to create
  // the corresponding phantomReq.
  bool is_created_trans_transmission_eligible = true;
  if (transaction_type == TransactionType::kPushUnsolicited &&
      ordering_mode == OrderingMode::kOrdered) {
    // Creates the phantomReq transaction.
    transactions[transaction_key]->CreatePacket(
        /*type=*/falcon::PacketType::kInvalid,  // Denotes a phantom request.
        /*direction=*/PacketDirection::kOutgoing,
        /*data_length=*/0, /*sgl_length=*/0, /*packet=*/nullptr);
    // Marks the enqueuing push unsolicited data as ineligible for transmission.
    // It can be transmitted only util the corresponding phantom request is
    // removed from the request queue.
    is_created_trans_transmission_eligible = false;
  }

  // For solicited push transaction: also setup the subsequent push solicited
  // data with the packet received from the ULP and create a Falcon internal
  // packet reflecting the push solicited request.
  if (transaction_type == TransactionType::kPushSolicited) {
    // Set the necessary fields (required by the Falcon resource manager).
    packet->falcon.rsn = rsn;
    packet->packet_type = falcon::PacketType::kPushSolicitedData;
    // Ordered Push Solicited Data should be marked ineligible for transmission
    // as it is enqueued in the connection scheduler along with the Push
    // Request. Not necessary for unordered as the data is enqueued only when we
    // get a grant.
    bool is_transmission_eligible =
        ordering_mode == OrderingMode::kOrdered ? false : true;
    transactions[transaction_key]->CreatePacket(
        falcon::PacketType::kPushSolicitedData, PacketDirection::kOutgoing,
        data_length, sgl_length, std::move(packet), is_transmission_eligible);
    // Reflects the push request packet.
    packet = std::make_unique<Packet>();
    packet_type = falcon::PacketType::kPushRequest;
    packet->metadata.scid = connection_metadata->scid;
  }

  transactions[transaction_key]->CreatePacket(
      packet_type, direction, data_length, sgl_length, std::move(packet),
      is_created_trans_transmission_eligible);
  transactions[transaction_key]->UpdateState(packet_type,
                                             TransactionStage::UlpRx, falcon);

  return {rsn, packet_type};
}

// Creates/updates transaction as Falcon receives a packet from the network.
// Also updates the transaction state and creates the appropriate packet
// metadata. Returns the expected request length (as per the original request)
// and the request/response length of the received Falcon packet.
NetworkTransactionInfo ConnectionState::CreateOrUpdateTransactionFromNetwork(
    std::unique_ptr<Packet> packet, FalconInterface* falcon) {
  uint32_t rsn = packet->falcon.rsn;
  PacketDirection direction = PacketDirection::kIncoming;
  TransactionType transaction_type;
  TransactionLocation location;
  uint16_t expected_length = 0;
  uint16_t actual_length = 0;
  uint16_t falcon_payload_length = packet->falcon.payload_length;
  const bool is_incoming_packet = true;
  TransactionKey transaction_key;
  switch (packet->packet_type) {
    case falcon::PacketType::kPullRequest: {
      transaction_type = TransactionType::kPull;
      location =
          GetTransactionLocation(packet->packet_type, is_incoming_packet);
      transaction_key = {rsn, location};
      // Use try_emplace to create the metadata only if does not exist
      // (otherwise it can overwrite existing metadata).
      auto [_, is_inserted] = transactions.try_emplace(
          transaction_key,
          CreateTransactionMetadata(version, rsn, transaction_type, location));
      if (is_inserted) {
        transactions[transaction_key]->request_length =
            packet->falcon.request_length;
      }
      break;
    }
    case falcon::PacketType::kPushRequest: {
      transaction_type = TransactionType::kPushSolicited;
      location =
          GetTransactionLocation(packet->packet_type, is_incoming_packet);
      transaction_key = {rsn, location};
      auto [_, is_inserted] = transactions.try_emplace(
          transaction_key,
          CreateTransactionMetadata(version, rsn, transaction_type, location));
      if (is_inserted) {
        transactions[transaction_key]->request_length =
            packet->falcon.request_length;
        // Create subsequent push grant packet (which is to be sent in response
        // to receiving a push request). Also, reflect the SSN from the request
        // to the grant.
        auto grant_packet = std::make_unique<Packet>();
        grant_packet->metadata.scid = connection_metadata->scid;
        grant_packet->packet_type = falcon::PacketType::kPushGrant;
        grant_packet->falcon.rsn = rsn;
        grant_packet->falcon.ssn = packet->falcon.ssn;
        transactions[transaction_key]->CreatePacket(
            falcon::PacketType::kPushGrant, PacketDirection::kOutgoing,
            std::move(grant_packet));
      }
      break;
    }
    case falcon::PacketType::kPushUnsolicitedData: {
      transaction_type = TransactionType::kPushUnsolicited;
      location =
          GetTransactionLocation(packet->packet_type, is_incoming_packet);
      transaction_key = {rsn, location};
      transactions.try_emplace(
          transaction_key,
          CreateTransactionMetadata(version, rsn, transaction_type, location));
      break;
    }
    case falcon::PacketType::kPullData:
      transaction_key = {
          rsn, GetTransactionLocation(packet->packet_type, is_incoming_packet)};
      actual_length = packet->falcon.payload_length;
      break;
    case falcon::PacketType::kPushGrant:
      transaction_key = {
          rsn, GetTransactionLocation(packet->packet_type, is_incoming_packet)};
      actual_length = packet->falcon.request_length;
      break;
    case falcon::PacketType::kPushSolicitedData:
      transaction_key = {
          rsn, GetTransactionLocation(packet->packet_type, is_incoming_packet)};
      actual_length = packet->falcon.payload_length;
      break;
    case falcon::PacketType::kResync:
      LOG(FATAL) << "Logic for handling resync packet not implemented.";
      break;
    case falcon::PacketType::kAck:
    case falcon::PacketType::kNack:
    case falcon::PacketType::kBack:
    case falcon::PacketType::kEack:
    case falcon::PacketType::kInvalid:
      LOG(FATAL) << "Attempt to create packet for invalid packet type.";
  }
  CHECK(transactions.contains(transaction_key));
  falcon::PacketType packet_type = packet->packet_type;
  transactions[transaction_key]->CreatePacket(
      packet_type, direction, falcon_payload_length, std::move(packet));
  transactions[transaction_key]->UpdateState(packet_type,
                                             TransactionStage::NtwkRx, falcon);
  expected_length = transactions[transaction_key]->request_length;
  return {expected_length, actual_length};
}

// Updates transaction state based on the packet type and transaction stage.
void TransactionMetadata::UpdateState(falcon::PacketType type,
                                      TransactionStage stage,
                                      FalconInterface* falcon) {
  absl::Duration now = falcon->get_environment()->ElapsedTime();
  double latency = absl::ToDoubleNanoseconds(now - last_state_update_timestamp);
  // Timestamp should only be updated when the transacion state is updated.
  // ACKs for pull request, push request and push grant do not update state.
  bool is_timestamp_update_eligible = true;
  auto collector = falcon->get_histogram_collector();
  switch (type) {
    case falcon::PacketType::kPullRequest:
      switch (stage) {
        case TransactionStage::UlpRx:
          // Beginning of a pull transaction (as initiator).
          transaction_start_timestamp = now;
          state = TransactionState::kPullReqUlpRx;
          break;
        case TransactionStage::UlpTx:
          // Either after network Rx, or after RNR.
          CHECK(state == TransactionState::kPullReqNtwkRx ||
                state == TransactionState::kPullReqRnrUlpRx);
          state = TransactionState::kPullReqUlpTx;
          collector->Add(PullLatencyTypes::kRxNetworkRequestToTxUlpRequest,
                         latency);
          break;
        case TransactionStage::UlpAckRx:
          CHECK(state == TransactionState::kPullReqUlpTx);
          state = TransactionState::kPullReqAckUlpRx;
          collector->Add(PullLatencyTypes::kTxUlpRequestToRxUlpRequestAck,
                         latency);
          break;
        case TransactionStage::UlpCompletionTx:
          LOG(FATAL) << "Cannot send a ULP completion for a pull request.";
          break;
        case TransactionStage::NtwkRx:
          // Beginning of a pull transaction (as target).
          state = TransactionState::kPullReqNtwkRx;
          break;
        case TransactionStage::NtwkTx:
          CHECK(state == TransactionState::kPullReqUlpRx);
          state = TransactionState::kPullReqNtwkTx;
          collector->Add(PullLatencyTypes::kRxUlpRequestToTxNetworkRequest,
                         latency);
          break;
        case TransactionStage::NtwkAckRx:
          // ACK for pull request does not update state hence should not update
          // timestamp.
          is_timestamp_update_eligible = false;
          break;
        case TransactionStage::UlpRnrRx:
          // Either get RNR from ULP or right after network Rx due to ordering.
          CHECK(state == TransactionState::kPullReqUlpTx ||
                state == TransactionState::kPullReqNtwkRx);
          state = TransactionState::kPullReqRnrUlpRx;
          break;
      }
      break;
    case falcon::PacketType::kPullData:
      switch (stage) {
        case TransactionStage::UlpRx:
          CHECK(state == TransactionState::kPullReqAckUlpRx);
          state = TransactionState::kPullDataUlpRx;
          collector->Add(PullLatencyTypes::kRxUlpRequestAckToRxUlpData,
                         latency);
          break;
        case TransactionStage::UlpTx:
          CHECK(state == TransactionState::kPullDataNtwkRx);
          state = TransactionState::kPullDataUlpTx;
          collector->Add(PullLatencyTypes::kRxNetworkDataToTxUlpData, latency);
          collector->Add(
              PullLatencyTypes::kTotalLatency,
              absl::ToDoubleNanoseconds(now - transaction_start_timestamp));
          break;
        case TransactionStage::UlpAckRx:
          LOG(FATAL) << "ULP does not send an explicit ACK for Pull Data.";
          break;
        case TransactionStage::UlpCompletionTx:
          LOG(FATAL) << "Cannot send a ULP completion for a pull data.";
          break;
        case TransactionStage::NtwkRx:
          CHECK(state == TransactionState::kPullReqNtwkTx);
          state = TransactionState::kPullDataNtwkRx;
          collector->Add(PullLatencyTypes::kTxNetworkRequestToRxNetworkData,
                         latency);
          break;
        case TransactionStage::NtwkTx:
          CHECK(state == TransactionState::kPullDataUlpRx);
          state = TransactionState::kPullDataNtwkTx;
          collector->Add(PullLatencyTypes::kRxUlpDataToTxNetworkData, latency);
          break;
        case TransactionStage::NtwkAckRx:
          CHECK(state == TransactionState::kPullDataNtwkTx);
          state = TransactionState::kPullDataAckNtwkRx;
          collector->Add(PullLatencyTypes::kTxNetworkDataToRxNetworkAck,
                         latency);
          break;
        case TransactionStage::UlpRnrRx:
          LOG(FATAL) << "Pull Data cannot be RNR NACKed by ULP.";
          break;
      }
      break;
    case falcon::PacketType::kPushRequest:
      switch (stage) {
        case TransactionStage::UlpRx:
          // Beginning of a push solicited transaction (as initiator).
          transaction_start_timestamp = now;
          state = TransactionState::kPushSolicitedReqUlpRx;
          break;
        case TransactionStage::UlpTx:
        case TransactionStage::UlpAckRx:
        case TransactionStage::UlpCompletionTx:
          LOG(FATAL) << "Falcon handles push requests internally.";
          break;
        case TransactionStage::NtwkRx:
          // Beginning of a push solicited transaction (as target).
          state = TransactionState::kPushReqNtwkRx;
          break;
        case TransactionStage::NtwkTx:
          CHECK(state == TransactionState::kPushSolicitedReqUlpRx);
          state = TransactionState::kPushReqNtwkTx;
          collector->Add(
              PushSolicitedLatencyTypes::kRxUlpDataToTxNetworkRequest, latency);
          break;
        case TransactionStage::NtwkAckRx:
          // ACK for push request does not update state hence should not update
          // timestamp.
          is_timestamp_update_eligible = false;
          break;
        case TransactionStage::UlpRnrRx:
          LOG(FATAL) << "Push Request cannot be RNR NACKed by ULP.";
          break;
      }
      break;
    case falcon::PacketType::kPushGrant:
      switch (stage) {
        case TransactionStage::UlpRx:
        case TransactionStage::UlpTx:
        case TransactionStage::UlpAckRx:
        case TransactionStage::UlpCompletionTx:
          LOG(FATAL) << "Falcon handles push grants internally.";
          break;
        case TransactionStage::NtwkRx:
          CHECK(state == TransactionState::kPushReqNtwkTx);
          state = TransactionState::kPushGrantNtwkRx;
          collector->Add(
              PushSolicitedLatencyTypes::kTxNetworkRequestToRxNetworkGrant,
              latency);
          break;
        case TransactionStage::NtwkTx:
          CHECK(state == TransactionState::kPushReqNtwkRx);
          state = TransactionState::kPushGrantNtwkTx;
          collector->Add(
              PushSolicitedLatencyTypes::kRxNetworkRequestToTxNetworkGrant,
              latency);
          break;
        case TransactionStage::NtwkAckRx:
          // ACK for push grant does not update state hence should not update
          // timestamp.
          is_timestamp_update_eligible = false;
          break;
        case TransactionStage::UlpRnrRx:
          LOG(FATAL) << "Push Grant cannot be RNR NACKed by ULP.";
          break;
      }
      break;
    case falcon::PacketType::kPushSolicitedData:
      switch (stage) {
        case TransactionStage::UlpRx:
          LOG(FATAL)
              << "Push Solicited data is generated internally by Falcon.";
          break;
        case TransactionStage::UlpTx:
          // Either after network Rx, or after RNR.
          CHECK(state == TransactionState::kPushSolicitedDataNtwkRx ||
                state == TransactionState::kPushSolicitedDataRnrUlpRx);
          state = TransactionState::kPushSolicitedDataUlpTx;
          collector->Add(PushSolicitedLatencyTypes::kRxNetworkDataToTxUlpData,
                         latency);
          break;
        case TransactionStage::UlpAckRx:
          CHECK(state == TransactionState::kPushSolicitedDataUlpTx);
          state = TransactionState::kPushSolicitedDataAckUlpRx;
          collector->Add(PushSolicitedLatencyTypes::kTxUlpDataToRxUlpAck,
                         latency);
          break;
        case TransactionStage::UlpCompletionTx:
          CHECK(state == TransactionState::kPushSolicitedDataAckNtwkRx);
          state = TransactionState::kPushCompletionUlpTx;
          collector->Add(
              PushSolicitedLatencyTypes::kRxNetworkAckToTxUlpCompletion,
              latency);
          collector->Add(
              PushSolicitedLatencyTypes::kTotalLatency,
              absl::ToDoubleNanoseconds(now - transaction_start_timestamp));
          break;
        case TransactionStage::NtwkRx:
          CHECK(state == TransactionState::kPushGrantNtwkTx);
          state = TransactionState::kPushSolicitedDataNtwkRx;
          collector->Add(
              PushSolicitedLatencyTypes::kTxNetworkGrantToRxNetworkData,
              latency);
          break;
        case TransactionStage::NtwkTx:
          CHECK(state == TransactionState::kPushGrantNtwkRx);
          state = TransactionState::kPushSolicitedDataNtwkTx;
          collector->Add(
              PushSolicitedLatencyTypes::kRxNetworkGrantToTxNetworkData,
              latency);
          break;
        case TransactionStage::NtwkAckRx:
          CHECK(state == TransactionState::kPushSolicitedDataNtwkTx);
          state = TransactionState::kPushSolicitedDataAckNtwkRx;
          collector->Add(
              PushSolicitedLatencyTypes::kTxNetworkDataToRxNetworkAck, latency);
          break;
        case TransactionStage::UlpRnrRx:
          // Either get RNR from ULP or right after network Rx due to ordering.
          CHECK(state == TransactionState::kPushSolicitedDataUlpTx ||
                state == TransactionState::kPushSolicitedDataNtwkRx);
          state = TransactionState::kPushSolicitedDataRnrUlpRx;
          break;
      }
      break;
    case falcon::PacketType::kPushUnsolicitedData:
      switch (stage) {
        case TransactionStage::UlpRx:
          // Beginning of a push unsolicited transaction (as initiator).
          transaction_start_timestamp = now;
          state = TransactionState::kPushUnsolicitedReqUlpRx;
          break;
        case TransactionStage::UlpTx:
          // Either after network Rx, or after RNR.
          CHECK(state == TransactionState::kPushUnsolicitedDataNtwkRx ||
                state == TransactionState::kPushUnsolicitedDataRnrUlpRx);
          state = TransactionState::kPushUnsolicitedDataUlpTx;
          collector->Add(PushUnsolicitedLatencyTypes::kRxNetworkDataToTxUlpData,
                         latency);
          break;
        case TransactionStage::UlpAckRx:
          CHECK(state == TransactionState::kPushUnsolicitedDataUlpTx);
          state = TransactionState::kPushUnsolicitedDataAckUlpRx;
          collector->Add(PushUnsolicitedLatencyTypes::kTxUlpDataToRxUlpAck,
                         latency);
          break;
        case TransactionStage::UlpCompletionTx:
          CHECK(state == TransactionState::kPushUnsolicitedDataAckNtwkRx);
          state = TransactionState::kPushCompletionUlpTx;
          collector->Add(
              PushUnsolicitedLatencyTypes::kRxNetworkAckToTxUlpCompletion,
              latency);
          collector->Add(
              PushUnsolicitedLatencyTypes::kTotalLatency,
              absl::ToDoubleNanoseconds(now - transaction_start_timestamp));
          break;
        case TransactionStage::NtwkRx:
          // Beginning of a push unsolicited transaction (as target).
          state = TransactionState::kPushUnsolicitedDataNtwkRx;
          break;
        case TransactionStage::NtwkTx:
          CHECK(state == TransactionState::kPushUnsolicitedReqUlpRx);
          state = TransactionState::kPushUnsolicitedDataNtwkTx;
          collector->Add(PushUnsolicitedLatencyTypes::kRxUlpDataToTxNetworkData,
                         latency);
          break;
        case TransactionStage::NtwkAckRx:
          CHECK(state == TransactionState::kPushUnsolicitedDataNtwkTx);
          state = TransactionState::kPushUnsolicitedDataAckNtwkRx;
          collector->Add(
              PushUnsolicitedLatencyTypes::kTxNetworkDataToRxNetworkAck,
              latency);
          break;
        case TransactionStage::UlpRnrRx:
          // Either get RNR from ULP or right after network Rx due to ordering.
          CHECK(state == TransactionState::kPushUnsolicitedDataUlpTx ||
                state == TransactionState::kPushUnsolicitedDataNtwkRx);
          state = TransactionState::kPushUnsolicitedDataRnrUlpRx;
          break;
      }
      break;
    case falcon::PacketType::kResync:
      LOG(FATAL) << "Logic for handling resync packet not implemented.";
      break;
    case falcon::PacketType::kAck:
    case falcon::PacketType::kNack:
    case falcon::PacketType::kBack:
    case falcon::PacketType::kEack:
    case falcon::PacketType::kInvalid:
      LOG(FATAL) << "Packets do not have associated state change.";
  }
  if (is_timestamp_update_eligible) {
    last_state_update_timestamp = now;
  }
}

};  // namespace isekai
