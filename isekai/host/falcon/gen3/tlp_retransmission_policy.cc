#include "isekai/host/falcon/gen3/tlp_retransmission_policy.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include "absl/time/time.h"
#include "glog/logging.h"
#include "isekai/common/constants.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/connection_scheduler_types.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"
#include "isekai/host/falcon/gen3/connection_state.h"
#include "isekai/host/falcon/gen3/falcon_types.h"

namespace isekai {

TlpRetransmissionPolicy::TlpRetransmissionPolicy(FalconModelInterface* falcon)
    : falcon_(falcon) {
  const auto early_retx = falcon_->get_config()->early_retx();

  if (early_retx.has_tlp_timeout_rtt_factor()) {
    tlp_timeout_rtt_factor_ = early_retx.tlp_timeout_rtt_factor();
  } else {
    LOG(FATAL) << "early_retx.tlp_timeout_rtt_factor not set in config.";
  }

  min_tlp_timeout_ = absl::Nanoseconds(early_retx.min_tlp_timeout_ns());
  per_window_probe_ = early_retx.tlp_per_window_probe();
  bypass_scan_range_limit_ = early_retx.tlp_bypass_scan_range_limit();

  if (early_retx.has_early_retx_threshold()) {
    early_retx_threshold_ = early_retx.early_retx_threshold();
  } else {
    early_retx_threshold_ = std::numeric_limits<uint32_t>::max();
  }

  if (early_retx.has_tlp_type()) {
    if (early_retx.tlp_type() != FalconConfig::EarlyRetx::FIRST_UNRECEIVED &&
        early_retx.tlp_type() != FalconConfig::EarlyRetx::LAST_UNRECEIVED) {
      LOG(FATAL) << "Unsupported TLP type: " << early_retx.tlp_type();
    }
    tlp_type_ = early_retx.tlp_type();
  } else {
    LOG(FATAL) << "early_retx.tlp_type not set in config.";
  }
}

// Returns the TLP timeout duration if a new TLP timer should be scheduled,
// otherwise ZeroDuration. TLP timeout is calculated based on RTT and TLP
// constants and stored in ConnectionState. If the new calculated value differs
// from TLP PTO in ConnectionState, the new event duration is returned and
// ConnectionState is updated; otherwise, the existing timeout still applies
// and ZeroDuration is returned.
absl::Duration TlpRetransmissionPolicy::GetTlpTimeoutDuration(
    ConnectionState* connection_state) {
  auto gen3_connection_state =
      static_cast<Gen3ConnectionState*>(connection_state);
  Gen3CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  absl::Duration rtt = falcon_->get_rate_update_engine()->FromFalconTimeUnits(
      ccmeta.gen3_flow_rtt_state[0]);
  absl::Duration tlp_timeout =
      (tlp_timeout_rtt_factor_ * rtt) +
      connection_state->connection_metadata->ack_coalescing_timeout;
  if (tlp_timeout < min_tlp_timeout_) {
    tlp_timeout = min_tlp_timeout_;
  }

  // Truncate fractional nanosecond value before scheduling event.
  tlp_timeout = absl::Trunc(tlp_timeout, absl::Nanoseconds(1));

  absl::Duration expiration_time =
      falcon_->get_environment()->ElapsedTime() + tlp_timeout;

  // Only schedule a new event if expiration time has changed.
  if (expiration_time == gen3_connection_state->tlp.pto) {
    return absl::ZeroDuration();
  }

  VLOG(2) << "[" << falcon_->get_host_id() << ": "
          << falcon_->get_environment()->ElapsedTime() << "]["
          << connection_state->connection_metadata->scid << ", -, -] "
          << " [SetupTlpTimer] TlpTimeout=" << tlp_timeout
          << " expiration_time=" << expiration_time;
  // Update the probe expiration timeout.
  gen3_connection_state->tlp.pto = expiration_time;
  return tlp_timeout;
}

// Calculates duration for next TLP timeout event upon retransmission setup.
RetransmissionPolicyAction TlpRetransmissionPolicy::SetupRetransmission(
    PacketMetadata* packet_metadata, ConnectionState* connection_state) {
  return {.scheduled_event_duration = GetTlpTimeoutDuration(connection_state)};
}

// Retrieves PacketMetadata given RSN and type, used by TLP to updated packet
// received state.
PacketMetadata* TlpRetransmissionPolicy::GetPacketMetadata(
    ConnectionState* connection_state, falcon::PacketType type, uint32_t rsn) {
  auto transaction = connection_state->GetTransaction(
      TransactionKey(rsn, GetTransactionLocation(type, /*incoming=*/false)));
  // If the transaction does not exist any more, skip.
  if (!transaction.ok()) {
    return nullptr;
  }
  auto packet_metadata = transaction.value()->GetPacketMetadata(type);
  if (!packet_metadata.ok()) {
    return nullptr;
  }
  return packet_metadata.value();
}

// Iterates over the request/data windows and updates packet received state.
//
void TlpRetransmissionPolicy::ApplyAckWindowMetadata(
    ConnectionState* connection_state,
    TransmitterReliabilityWindowMetadata* tx_window, uint32_t base_psn,
    const FalconAckPacketBitmap& ack_bitmap) {
  for (uint32_t index = 0; index < ack_bitmap.Size(); index++) {
    uint32_t psn = index + base_psn;
    auto packet_context = tx_window->GetOutstandingPacketRSN(psn);
    if (!packet_context.ok()) {
      continue;
    }

    uint32_t rsn = packet_context.value()->rsn;
    auto type = packet_context.value()->packet_type;
    PacketMetadata* packet_metadata =
        GetPacketMetadata(connection_state, type, rsn);
    if (packet_metadata == nullptr) {
      continue;
    }
    if (ack_bitmap.Get(index) && !packet_metadata->received) {
      packet_metadata->received = true;
    }
  }
}

// Iterates over request/data windows in increasing PSN order, finding the
// first unreceived packet in each window if per_window_probe_ is true.
// Otherwise, finds the earliest unreceived packet across both windows.
// Probe packets to be retransmitted are appended to retx_work_ids.
void TlpRetransmissionPolicy::GetFirstUnreceivedProbes(
    ConnectionState* connection_state,
    std::array<TransmitterReliabilityWindowMetadata*, 2> tx_windows,
    std::vector<RetransmissionWorkId>& retx_work_ids) {
  std::vector<PacketMetadata*> per_window_probe_metadata;
  std::vector<RetransmissionWorkId> per_window_probe_retx;

  for (auto& tx_window : tx_windows) {
    for (auto& pkt : tx_window->outstanding_packets) {
      PacketMetadata* packet_metadata =
          GetPacketMetadata(connection_state, pkt.type, pkt.rsn);
      if (packet_metadata == nullptr) {
        continue;
      }
      if (packet_metadata->early_retx_attempts >= early_retx_threshold_) {
        // Stop scan upon reaching a packet which exceeds early retx threshold.
        break;
      }

      if (!packet_metadata->received) {
        per_window_probe_metadata.push_back(packet_metadata);
        per_window_probe_retx.push_back(pkt);
        // Stop window scan after finding an unreceived packet.
        break;
      }
    }
  }
  SelectProbesBetweenWindows(per_window_probe_metadata, per_window_probe_retx,
                             retx_work_ids);
}

// Iterates over request/data windows in decreasing PSN order, finding the
// last unreceived packet in each window if per_window_probe_ is true.
// Otherwise, finds the latest unreceived packet across both windows. Packets
// outside of the ACK bitmap range are ignored by the scan unless config option
// tlp_bypass_scan_range_limit is set. Probe packets selected for
// retransmission are appended to retx_work_ids.
void TlpRetransmissionPolicy::GetLastUnreceivedProbes(
    ConnectionState* connection_state,
    std::array<TransmitterReliabilityWindowMetadata*, 2> tx_windows,
    std::vector<RetransmissionWorkId>& retx_work_ids) {
  auto gen3_connection_state =
      static_cast<Gen3ConnectionState*>(connection_state);
  std::vector<PacketMetadata*> per_window_probe_metadata;
  std::vector<RetransmissionWorkId> per_window_probe_retx;

  // Last-unreceived scan must select a packet within range of ACK bitmap given
  // the maximum received ACK BPSN.
  std::array<uint32_t, 2> scan_limit = {
      gen3_connection_state->max_req_ack_bpsn + kAckPacketRequestWindowSize,
      gen3_connection_state->max_data_ack_bpsn + kAckPacketDataWindowSize,
  };

  for (int i = 0; i < tx_windows.size(); ++i) {
    auto tx_window = tx_windows[i];
    uint32_t window_scan_limit = scan_limit[i];

    auto& outstanding = tx_window->outstanding_packets;
    for (auto it = outstanding.rbegin(); it != outstanding.rend(); ++it) {
      const RetransmissionWorkId& pkt = *it;
      if (!bypass_scan_range_limit_ && pkt.psn >= window_scan_limit) {
        // If packet is outside ACK bitmap range, it is not eligible for retx.
        // Continue scan to find any outstanding in-range PSN.
        continue;
      }
      PacketMetadata* packet_metadata =
          GetPacketMetadata(connection_state, pkt.type, pkt.rsn);
      if (packet_metadata == nullptr) {
        continue;
      }
      if (packet_metadata->early_retx_attempts >= early_retx_threshold_) {
        // Stop scan upon reaching a packet which exceeds early retx threshold.
        break;
      }

      if (!packet_metadata->received) {
        per_window_probe_metadata.push_back(packet_metadata);
        per_window_probe_retx.push_back(pkt);
        // Stop window scan after finding an unreceived packet.
        break;
      }
    }
  }

  SelectProbesBetweenWindows(per_window_probe_metadata, per_window_probe_retx,
                             retx_work_ids);
}

// With any qualifying packet from each window in per_window_probe_metadata and
// per_window_probe_retx, selects up to 2 probes using RSN-based decision if
// per_window_probe is false, or one probe per window if per_window_probe is
// true. If only one packet qualifies, selects that packet. Writes selected
// probes to retx_work_ids and sets retransmission_reason to kEarlyTlp.
void TlpRetransmissionPolicy::SelectProbesBetweenWindows(
    std::vector<PacketMetadata*> per_window_probe_metadata,
    std::vector<RetransmissionWorkId> per_window_probe_retx,
    std::vector<RetransmissionWorkId>& retx_work_ids) {
  if (per_window_probe_ || per_window_probe_retx.size() <= 1) {
    // In this case, no need to choose a probe between both windows, since at
    // most one probe meets criteria or per_window_probe is enabled.
    // Perform all retransmissions in per_window_probe_retx.
    for (auto& packet_metadata : per_window_probe_metadata) {
      packet_metadata->retransmission_reason = RetransmitReason::kEarlyTlp;
    }
    retx_work_ids.insert(retx_work_ids.end(), per_window_probe_retx.begin(),
                         per_window_probe_retx.end());
  } else {
    // In this case, there must be a qualifying probe in both windows.
    DCHECK_EQ(per_window_probe_retx.size(), 2);
    DCHECK_EQ(per_window_probe_metadata.size(), 2);

    // If both windows contain an unreceived packet, choose data window packet
    // as probe if packet type is PullData or PushGrant, or if RSN is lower.
    // Otherwise, choose request window packet as probe.
    if (per_window_probe_metadata[1]->type == falcon::PacketType::kPullData ||
        per_window_probe_metadata[1]->type == falcon::PacketType::kPushGrant ||
        per_window_probe_retx[1].rsn < per_window_probe_retx[0].rsn) {
      per_window_probe_metadata[1]->retransmission_reason =
          RetransmitReason::kEarlyTlp;
      retx_work_ids.push_back(per_window_probe_retx[1]);
    } else {
      retx_work_ids.push_back(per_window_probe_retx[0]);
      per_window_probe_metadata[0]->retransmission_reason =
          RetransmitReason::kEarlyTlp;
    }
  }
}

// Upon receiving an ACK, iterates over the request/data window, updating
// received state for each packet. Calculates and returns duration for next TLP
// timeout event if necessary to schedule a new event.
RetransmissionPolicyAction TlpRetransmissionPolicy::HandleAck(
    const Packet* packet, ConnectionState* connection_state) {
  if (packet->ack.ack_type == Packet::Ack::kEack) {
    auto gen3_connection_state =
        static_cast<Gen3ConnectionState*>(connection_state);
    // Iterate over request window and apply received state.
    ApplyAckWindowMetadata(
        connection_state,
        &connection_state->tx_reliability_metadata.request_window_metadata,
        packet->ack.rrbpsn, packet->ack.receiver_request_bitmap);
    // Iterate over data window and apply received state.
    ApplyAckWindowMetadata(
        connection_state,
        &connection_state->tx_reliability_metadata.data_window_metadata,
        packet->ack.rdbpsn, packet->ack.received_bitmap);

    // Update max_req_ack_bpsn and max_data_ack_bpsn to set RACK scan limits.
    gen3_connection_state->max_req_ack_bpsn =
        std::max(gen3_connection_state->max_req_ack_bpsn, packet->ack.rrbpsn);
    gen3_connection_state->max_data_ack_bpsn =
        std::max(gen3_connection_state->max_data_ack_bpsn, packet->ack.rdbpsn);
  }
  return {.scheduled_event_duration = GetTlpTimeoutDuration(connection_state)};
}

// Upon NACK, calculates and returns duration for next TLP timeout event if
// necessary to schedule a new event.
RetransmissionPolicyAction TlpRetransmissionPolicy::HandleNack(
    const Packet* packet, ConnectionState* connection_state) {
  return {.scheduled_event_duration = GetTlpTimeoutDuration(connection_state)};
}

// Upon implicit ACK, calculates and returns duration for next TLP timeout event
// if necessary to schedule a new event.
RetransmissionPolicyAction TlpRetransmissionPolicy::HandleImplicitAck(
    const Packet* packet, ConnectionState* connection_state) {
  return {.scheduled_event_duration = GetTlpTimeoutDuration(connection_state)};
}

// Upon piggybacked ACK, calculates and returns duration for next TLP timeout
// event if necessary to schedule a new event.
RetransmissionPolicyAction TlpRetransmissionPolicy::HandlePiggybackedAck(
    const Packet* packet, ConnectionState* connection_state) {
  return {.scheduled_event_duration = GetTlpTimeoutDuration(connection_state)};
}

// Handles TLP timeout if timeout duration is still valid (latest TLP PTO). If
// valid, iterates over request and data window, selecting any probe packets
// based on config criteria. If tlp_type is FIRST_UNRECEIVED, selects the first
// unreceived packet in each window. If tlp_type is LAST_UNRECEIVED, selects the
// last unreceived packet in each window, ordered by PSN. If the config option
// tlp_per_window_probe is set, applies the criteria to each window separately,
// returning up to two probe packets for retransmission. Otherwise, at most one
// probe will occur, selected based on the earliest (FIRST_UNRECEIVED case) or
// latest (LAST_UNRECEIVED case) transmit time of the probe from each window.
RetransmissionPolicyAction TlpRetransmissionPolicy::HandleScheduledEvent(
    ConnectionState* connection_state) {
  auto gen3_connection_state =
      static_cast<Gen3ConnectionState*>(connection_state);
  if (!falcon_->get_environment()->ElapsedTimeEquals(
          gen3_connection_state->tlp.pto)) {
    // Skip if timeout is no longer applicable.
    return NoOpAction();
  }

  {
    Gen3CongestionControlMetadata& ccmeta =
        CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
            *connection_state->congestion_control_metadata);
    VLOG(2) << "[" << falcon_->get_host_id() << ": "
            << falcon_->get_environment()->ElapsedTime() << "]["
            << connection_state->connection_metadata->scid << ", -, -] "
            << " [HandleTlpTimeout] rtt="
            << falcon_->get_rate_update_engine()->FromFalconTimeUnits(
                   ccmeta.gen3_flow_rtt_state[0]);
  }

  // Clear TLP PTO until next TLP event is scheduled.
  gen3_connection_state->tlp.pto = absl::ZeroDuration();

  std::array<TransmitterReliabilityWindowMetadata*, 2> tx_windows{
      &connection_state->tx_reliability_metadata.request_window_metadata,
      &connection_state->tx_reliability_metadata.data_window_metadata};

  std::vector<RetransmissionWorkId> retx_work_ids;
  if (tlp_type_ == FalconConfig::EarlyRetx::FIRST_UNRECEIVED) {
    GetFirstUnreceivedProbes(connection_state, tx_windows, retx_work_ids);
  } else if (tlp_type_ == FalconConfig::EarlyRetx::LAST_UNRECEIVED) {
    GetLastUnreceivedProbes(connection_state, tx_windows, retx_work_ids);
  } else {
    LOG(FATAL) << "Unsupported TLP type: " << tlp_type_;
  }
  for (auto& retx_work_id : retx_work_ids) {
    VLOG(2) << "[" << falcon_->get_host_id() << ": "
            << falcon_->get_environment()->ElapsedTime() << "]["
            << connection_state->connection_metadata->scid << ", -, -] "
            << "TLP retx for packet " << retx_work_id.rsn << " PSN "
            << retx_work_id.psn
            << " at now=" << falcon_->get_environment()->ElapsedTime();
  }
  return {.retx_work_ids = retx_work_ids};
}

}  // namespace isekai
