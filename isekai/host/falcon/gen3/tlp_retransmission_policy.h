#ifndef ISEKAI_HOST_FALCON_GEN3_TLP_RETRANSMISSION_POLICY_H_
#define ISEKAI_HOST_FALCON_GEN3_TLP_RETRANSMISSION_POLICY_H_

#include <array>
#include <cstdint>
#include <vector>

#include "absl/time/time.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/connection_scheduler_types.h"
#include "isekai/host/falcon/gen1/falcon_types.h"

namespace isekai {

using RetransmissionPolicyAction =
    RetransmissionPolicy::RetransmissionPolicyAction;

class TlpRetransmissionPolicy : public RetransmissionPolicy {
 public:
  explicit TlpRetransmissionPolicy(FalconModelInterface* falcon);

  RetransmissionPolicyAction SetupRetransmission(
      PacketMetadata* packet_metadata,
      ConnectionState* connection_state) override;

  // Handle ACK by updating TLP state and resetting timer.
  RetransmissionPolicyAction HandleAck(
      const Packet* packet, ConnectionState* connection_state) override;

  // Handle NACK by updating TLP state and resetting timer.
  RetransmissionPolicyAction HandleNack(
      const Packet* packet, ConnectionState* connection_state) override;

  // Handle ACK by updating TLP state and resetting timer.
  RetransmissionPolicyAction HandleImplicitAck(
      const Packet* packet, ConnectionState* connection_state) override;

  // Handle ACK by updating TLP state and resetting timer.
  RetransmissionPolicyAction HandlePiggybackedAck(
      const Packet* packet, ConnectionState* connection_state) override;

  // Handle TLP timeout, returning retx packet if applicable.
  RetransmissionPolicyAction HandleScheduledEvent(
      ConnectionState* connection_state) override;

  // Handler functions below are unused by TLP.
  RetransmissionPolicyAction InitConnection(
      ConnectionState* connection_state) override {
    return NoOpAction();
  }

  RetransmissionPolicyAction HandleAckToUlp(
      PacketMetadata* packet_metadata,
      ConnectionState* connection_state) override {
    return NoOpAction();
  }

  RetransmissionPolicyAction HandleRetransmitTimeoutEvent(
      ConnectionState* connection_state) override {
    return NoOpAction();
  }

  absl::Duration GetTimeoutOfPacket(ConnectionState* connection_state,
                                    PacketMetadata* packet_metadata) override {
    return absl::InfiniteDuration();
  }

 private:
  // Retrieves PacketMetadata, or nullptr if transaction no longer exists.
  PacketMetadata* GetPacketMetadata(ConnectionState* connection_state,
                                    falcon::PacketType type, uint32_t rsn);

  // Sets received status in PacketMetadata given a tx_window and ACK bitmap.
  void ApplyAckWindowMetadata(ConnectionState* connection_state,
                              TransmitterReliabilityWindowMetadata* tx_window,
                              uint32_t base_psn,
                              const FalconAckPacketBitmap& ack_bitmap);

  // Returns duration for next TLP event to be scheduled, or ZeroDuration if no
  // new event should be scheduled.
  absl::Duration GetTlpTimeoutDuration(ConnectionState* connection_state);

  // Iterates over request/data windows in increasing PSN order, finding the
  // first unreceived packet in each window if per_window_probe_ is true.
  // Otherwise, finds the earliest unreceived packet across both windows.
  // Probe packets to be retransmitted are appended to retx_work_ids.
  void GetFirstUnreceivedProbes(
      ConnectionState* connection_state,
      std::array<TransmitterReliabilityWindowMetadata*, 2> tx_windows,
      std::vector<RetransmissionWorkId>& retx_work_ids);

  // Iterates over request/data windows in decreasing PSN order, finding the
  // last unreceived packet in each window if per_window_probe_ is true.
  // Otherwise, finds the latest unreceived packet across both windows. Packets
  // outside of the ACK bitmap range are ignored by the scan. Probe packets
  // selected for retransmission are appended to retx_work_ids.
  void GetLastUnreceivedProbes(
      ConnectionState* connection_state,
      std::array<TransmitterReliabilityWindowMetadata*, 2> tx_windows,
      std::vector<RetransmissionWorkId>& retx_work_ids);

  // With any qualifying packet from each window in per_window_probe_metadata
  // and per_window_probe_retx, selects up to 2 probes using RSN-based decision
  // if per_window_probe is false, or one probe per window if per_window_probe
  // is true. If only one packet qualifies, selects that packet. Writes selected
  // probes to retx_work_ids and sets retransmission_reason to kEarlyTlp.
  void SelectProbesBetweenWindows(
      std::vector<PacketMetadata*> per_window_probe_metadata,
      std::vector<RetransmissionWorkId> per_window_probe_retx,
      std::vector<RetransmissionWorkId>& retx_work_ids);

  FalconModelInterface* const falcon_;

  // These three variables are used for TLP calculation. With swift enabled,
  // TLP calculations are performed within RUE and these variables are unused.
  // Minimum PTO for TLP retransmission.
  absl::Duration min_tlp_timeout_;
  // PTO is calculated using rtt * this factor.
  double tlp_timeout_rtt_factor_;
  // TLP retransmissions are subject to maximum early retx threshold.

  uint32_t early_retx_threshold_;
  // If true, send one probe per window. Otherwise, single probe will be sent
  // across both windows, selecting the earliest overall transmit timestamp.
  bool per_window_probe_;

  // If true, disables scan range limit when TLP LAST_UNRECEIVED probe selection
  // policy is set. TLP will use the last unreceived packet across the entire TX
  // window. Otherwise, by default, TLP probe will be the last unreceived packet
  // within range of the ACK request/data bitmaps.
  bool bypass_scan_range_limit_;

  // Probe selection option, either first or last unreceived packet.
  FalconConfig::EarlyRetx::TlpType tlp_type_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN3_TLP_RETRANSMISSION_POLICY_H_
