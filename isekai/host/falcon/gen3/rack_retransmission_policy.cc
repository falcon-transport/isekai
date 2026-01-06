#include "isekai/host/falcon/gen3/rack_retransmission_policy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "absl/time/time.h"
#include "glog/logging.h"
#include "isekai/common/constants.h"
#include "isekai/common/packet.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/connection_scheduler_types.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"
#include "isekai/host/falcon/gen3/connection_state.h"
#include "isekai/host/falcon/gen3/falcon_types.h"

namespace isekai {

RackRetransmissionPolicy::RackRetransmissionPolicy(FalconModelInterface* falcon)
    : falcon_(falcon) {
  const auto early_retx = falcon_->get_config()->early_retx();
  if (early_retx.has_rack_time_window_rtt_factor()) {
    rack_time_window_rtt_factor_ = early_retx.rack_time_window_rtt_factor();
  } else {
    LOG(FATAL) << "Missing rack_time_window_rtt_factor in config.";
  }

  min_rack_time_window_ =
      absl::Nanoseconds(early_retx.min_rack_time_window_ns());

  if (early_retx.has_early_retx_threshold()) {
    early_retx_threshold_ = early_retx.early_retx_threshold();
  } else {
    early_retx_threshold_ = std::numeric_limits<uint32_t>::max();
  }

  bypass_rto_check_ = early_retx.rack_bypass_rto_check();
  bypass_scan_range_limit_ = early_retx.rack_bypass_scan_range_limit();
}

// Upon receiving an ACK, updates RACK state with T1 and iterates over the
// request/data window, updating received state for each packet. For EACKs,
// iterates over the request/data window and retransmits packets that meet RACK
// criteria.
RetransmissionPolicyAction RackRetransmissionPolicy::HandleAck(
    const Packet* packet, ConnectionState* connection_state) {
  auto gen3_connection_state =
      static_cast<Gen3ConnectionState*>(connection_state);
  RackUpdate(connection_state, packet->ack.timestamp_1);
  if (packet->ack.ack_type == Packet::Ack::kEack) {
    // Iterate over request window and update received state.
    ApplyAckWindowMetadata(
        connection_state,
        &connection_state->tx_reliability_metadata.request_window_metadata,
        packet->ack.rrbpsn, packet->ack.receiver_request_bitmap);
    // Iterate over data window and update received state.
    ApplyAckWindowMetadata(
        connection_state,
        &connection_state->tx_reliability_metadata.data_window_metadata,
        packet->ack.rdbpsn, packet->ack.received_bitmap);
    auto gen3_connection_state =
        static_cast<Gen3ConnectionState*>(connection_state);
    // Update max_req_ack_bpsn and max_data_ack_bpsn to set RACK scan limits.
    gen3_connection_state->max_req_ack_bpsn =
        std::max(gen3_connection_state->max_req_ack_bpsn, packet->ack.rrbpsn);
    gen3_connection_state->max_data_ack_bpsn =
        std::max(gen3_connection_state->max_data_ack_bpsn, packet->ack.rdbpsn);

    // Perform EACK-initiated RACK scan on both request and data windows.
    gen3_connection_state->rack.rack_enabled_for_window[0] = true;
    gen3_connection_state->rack.rack_enabled_for_window[1] = true;

    const RetransmissionPolicyAction& rack_retx_actions =
        PerformRackScan(connection_state);

    return rack_retx_actions;
  } else if (packet->ack.ack_type == Packet::Ack::kAck) {
    // For a base ACK, disable RACK scan if BPSN is greater than the highest
    // received EACK BPSN set in max_{req,data}_ack_bpsn.
    if (packet->ack.rrbpsn > gen3_connection_state->max_req_ack_bpsn) {
      gen3_connection_state->rack.rack_enabled_for_window[0] = false;
    }
    if (packet->ack.rdbpsn > gen3_connection_state->max_data_ack_bpsn) {
      gen3_connection_state->rack.rack_enabled_for_window[1] = false;
    }
  }
  return NoOpAction();
}

// Upon receiving a NACK, updates RACK state with T1.
RetransmissionPolicyAction RackRetransmissionPolicy::HandleNack(
    const Packet* packet, ConnectionState* connection_state) {
  RackUpdate(connection_state, packet->nack.timestamp_1);
  return NoOpAction();
}

// Upon RTO timeout, iterates over the request/data window and retransmits
// packets that meet RACK criteria. The packets returned by this function for
// retransmission are those which do not meet the base RTO criteria. Regular RTO
// retransmissions are handled subsequently by ReliabilityManager, so skipping
// them here avoids duplicate retransmissions.
RetransmissionPolicyAction
RackRetransmissionPolicy::HandleRetransmitTimeoutEvent(
    ConnectionState* connection_state) {
  if (bypass_rto_check_) {
    // In this simplified RACK mode, RACK scans occur upon ACK and not as
    // background events.
    return NoOpAction();
  }
  return PerformRackScan(connection_state);
}

// Returns the RACK RTO for a given packet, if applicable to a packet, otherwise
// InfiniteDuration. This function is used by ReliabilityManager to schedule
// the RTO events, at the minimum duration between RTO and the value returned by
// all RetransmissionPolicy instances.
absl::Duration RackRetransmissionPolicy::GetTimeoutOfPacket(
    ConnectionState* connection_state, PacketMetadata* packet_metadata) {
  auto gen3_connection_state =
      static_cast<Gen3ConnectionState*>(connection_state);
  if (bypass_rto_check_) {
    // In this simplified RACK mode, RACK scans occur upon ACK and not as
    // background events. Packet timeout is unaffected by RACK.
    return absl::InfiniteDuration();
  }

  if (!packet_metadata->received &&
      packet_metadata->transmission_time <=
          gen3_connection_state->rack.xmit_ts &&
      packet_metadata->early_retx_attempts < early_retx_threshold_) {
    uint32_t scan_range_limit;
    bool rack_enabled_for_window;
    if (packet_metadata->type == falcon::PacketType::kPushRequest ||
        packet_metadata->type == falcon::PacketType::kPullRequest) {
      scan_range_limit =
          gen3_connection_state->max_req_ack_bpsn + kAckPacketRequestWindowSize;
      rack_enabled_for_window =
          gen3_connection_state->rack.rack_enabled_for_window[0];
    } else {
      scan_range_limit =
          gen3_connection_state->max_data_ack_bpsn + kAckPacketDataWindowSize;
      rack_enabled_for_window =
          gen3_connection_state->rack.rack_enabled_for_window[1];
    }
    if ((bypass_scan_range_limit_ || packet_metadata->psn < scan_range_limit) &&
        rack_enabled_for_window) {
      return gen3_connection_state->rack.rto;
    }
  }
  // If RACK is not applicable, return InfiniteDuration such that min timeout
  // calculation will be unaffected by RACK.
  return absl::InfiniteDuration();
}

// Recalculates rack.rto and rack.xmit_ts within ConnectionState given a
// tx_time, the current elapsed time, and RTT.
void RackRetransmissionPolicy::RackUpdate(ConnectionState* connection_state,
                                          absl::Duration tx_time) {
  auto gen3_connection_state =
      static_cast<Gen3ConnectionState*>(connection_state);
  if (tx_time > gen3_connection_state->rack.xmit_ts) {
    gen3_connection_state->rack.xmit_ts = tx_time;
    gen3_connection_state->rack.rto =
        falcon_->get_environment()->ElapsedTime() - tx_time +
        CalculateRackWindow(gen3_connection_state);
    // There is no need to Reschedule the retransmit timer, because RackUpdate
    // is called within ACK handling (HandleAck, HandleNack,
    // HandlePiggybackedACK, HandleImplicitAck), all of which reschedules
    // retransmit timer at the end.
  }
  VLOG(2) << "[" << falcon_->get_host_id() << ": "
          << falcon_->get_environment()->ElapsedTime() << "]["
          << connection_state->connection_metadata->scid << ", -, -] "
          << " [RackUpdate] tx_time=" << tx_time
          << " rack.xmit_ts=" << gen3_connection_state->rack.xmit_ts
          << " rack.rto=" << gen3_connection_state->rack.rto;
}

// Computes RACK time window based on RTT * rack_time_window_rtt_factor and
// min_rack_time_window specified in config.
absl::Duration RackRetransmissionPolicy::CalculateRackWindow(
    ConnectionState* connection_state) {
  Gen3CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  auto rack_time_window =
      falcon_->get_rate_update_engine()->FromFalconTimeUnits(
          ccmeta.gen3_flow_rtt_state[0]) *
      rack_time_window_rtt_factor_;
  rack_time_window =
      absl::Nanoseconds(std::ceil(absl::ToDoubleNanoseconds(rack_time_window)));
  if (rack_time_window < min_rack_time_window_) return min_rack_time_window_;
  return rack_time_window;
}

// Retrieves PacketMetadata, used by RACK to store received state.
PacketMetadata* RackRetransmissionPolicy::GetPacketMetadata(
    ConnectionState* const connection_state, uint32_t rsn,
    falcon::PacketType type) {
  // Get a handle on the transaction state.
  CHECK_OK_THEN_ASSIGN(
      auto transaction,
      connection_state->GetTransaction(TransactionKey(
          rsn, GetTransactionLocation(type, /*incoming=*/false))));
  CHECK_OK_THEN_ASSIGN(PacketMetadata * packet_metadata,
                       transaction->GetPacketMetadata(type));
  return packet_metadata;
}

// Upon receiving an EACK, iterates over the request/data window and updates
// received state for each packet.
void RackRetransmissionPolicy::ApplyAckWindowMetadata(
    ConnectionState* connection_state,
    TransmitterReliabilityWindowMetadata* tx_window, uint32_t base_psn,
    const FalconAckPacketBitmap& ack_bitmap) {
  for (uint32_t index = 0; index < ack_bitmap.Size(); index++) {
    uint32_t psn = index + base_psn;
    auto packet_context = tx_window->GetOutstandingPacketRSN(psn);
    if (!packet_context.ok()) continue;

    PacketMetadata* packet_metadata =
        GetPacketMetadata(connection_state, packet_context.value()->rsn,
                          packet_context.value()->packet_type);
    if (ack_bitmap.Get(index) && !packet_metadata->received) {
      packet_metadata->received = true;
    }
  }
}

// Iterates over the request/data window and retransmits packets that meet RACK
// criteria. The packets returned by this function for retransmission are those
// which do not meet the base RTO criteria. On a timeout event, base RTO
// retransmissions are handled subsequently by ReliabilityManager, so skipping
// them here avoids duplicate retransmissions. Scanning terminates when a non-
// received packet fails the RACK xmit_ts check, or when a PSN is reached that
// exceeds the scan limit value for the corresponding request/data window.
RetransmissionPolicyAction RackRetransmissionPolicy::PerformRackScan(
    ConnectionState* connection_state) {
  auto gen3_connection_state =
      static_cast<Gen3ConnectionState*>(connection_state);
  std::vector<RetransmissionWorkId> retx_list;
  std::array<TransmitterReliabilityWindowMetadata*, 2> tx_windows{
      &gen3_connection_state->tx_reliability_metadata.request_window_metadata,
      &gen3_connection_state->tx_reliability_metadata.data_window_metadata};

  std::array<uint32_t, 2> scan_limit = {
      gen3_connection_state->max_req_ack_bpsn + kAckPacketRequestWindowSize,
      gen3_connection_state->max_data_ack_bpsn + kAckPacketDataWindowSize,
  };

  const absl::Duration& rack_rto = gen3_connection_state->rack.rto;
  CongestionControlMetadata& ccmeta =
      *connection_state->congestion_control_metadata;
  const absl::Duration& base_rto = ccmeta.retransmit_timeout;

  // If RACK timeout exceeds base RTO, any retransmissions will be performed in
  // base RTO scan.
  if (!bypass_rto_check_ && rack_rto > base_rto) {
    return NoOpAction();
  }

  for (int i = 0; i < tx_windows.size(); ++i) {
    auto& tx_window = tx_windows[i];
    uint32_t window_scan_limit = scan_limit[i];

    if (!gen3_connection_state->rack.rack_enabled_for_window[i]) {
      continue;
    }

    for (auto& work_id : tx_window->outstanding_packets) {
      gen3_connection_state->rack_pkt_scan_count++;

      int bitmap_index =
          (work_id.psn - tx_window->base_packet_sequence_number) %
          tx_window->window->Size();
      if (tx_window->window->Get(bitmap_index)) {
        continue;
      }

      if (!bypass_scan_range_limit_ && work_id.psn >= window_scan_limit) {
        break;
      }

      PacketMetadata* packet_metadata =
          GetPacketMetadata(connection_state, work_id.rsn, work_id.type);
      CHECK_NE(packet_metadata, nullptr);

      if (packet_metadata->received ||
          packet_metadata->early_retx_attempts >= early_retx_threshold_) {
        //
        // when the limit is reached.
        // Skip over received packets or packets exceeding early retx threshold.
        continue;
      }

      auto packet_elapsed_time = falcon_->get_environment()->ElapsedTime() -
                                 packet_metadata->transmission_time;

      if (bypass_rto_check_) {
        // Simplified RACK.
        if (packet_metadata->transmission_time <=
            gen3_connection_state->rack.xmit_ts) {
          VLOG(1) << "[" << falcon_->get_host_id() << ": "
                  << falcon_->get_environment()->ElapsedTime() << "]["
                  << gen3_connection_state->connection_metadata->scid
                  << ", -, -] RACK retx for packet " << work_id.rsn << " PSN "
                  << work_id.psn << " with elapsed time " << packet_elapsed_time
                  << " and RACK RTO " << rack_rto
                  << " at now=" << falcon_->get_environment()->ElapsedTime();
          packet_metadata->retransmission_reason = RetransmitReason::kEarlyRack;
          retx_list.emplace_back(work_id.rsn, work_id.psn, work_id.type);
        }
      } else {
        // Standard RACK.
        if (packet_metadata->transmission_time <=
            gen3_connection_state->rack.xmit_ts) {
          if (packet_elapsed_time >= rack_rto) {
            VLOG(1) << "[" << falcon_->get_host_id() << ": "
                    << falcon_->get_environment()->ElapsedTime() << "]["
                    << connection_state->connection_metadata->scid
                    << ", -, -] RACK retx for packet " << work_id.rsn << " PSN "
                    << work_id.psn << " with elapsed time "
                    << packet_elapsed_time << " and RACK RTO " << rack_rto
                    << " at now=" << falcon_->get_environment()->ElapsedTime();
            packet_metadata->retransmission_reason =
                RetransmitReason::kEarlyRack;
            retx_list.emplace_back(work_id.rsn, work_id.psn, work_id.type);
          } else {
            // Exit RACK scan when time check fails.
            break;
          }
        }
      }
    }
  }

  return RetransmissionPolicyAction{.retx_work_ids = retx_list};
}

}  // namespace isekai
