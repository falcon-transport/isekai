#ifndef ISEKAI_HOST_FALCON_GEN3_RACK_RETRANSMISSION_POLICY_H_
#define ISEKAI_HOST_FALCON_GEN3_RACK_RETRANSMISSION_POLICY_H_

#include <cstdint>

#include "absl/time/time.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"

namespace isekai {

using RetransmissionPolicyAction =
    RetransmissionPolicy::RetransmissionPolicyAction;

class RackRetransmissionPolicy : public RetransmissionPolicy {
 public:
  explicit RackRetransmissionPolicy(FalconModelInterface* falcon);

  // Handle ACK by updating RACK state and for EACKs, returning retx work IDs
  // which meet RACK criteria.
  RetransmissionPolicyAction HandleAck(
      const Packet* packet, ConnectionState* connection_state) override;

  // Handle NACK by updating RACK state.
  RetransmissionPolicyAction HandleNack(
      const Packet* packet, ConnectionState* connection_state) override;

  // Handle timeout event by returning retransmission work IDs which meet RACK.
  // criteria. This event occurs when the minimum timeout duration provided by
  // GetTimeoutOfPacket across all retransmission policies (including RTO) has
  // elapsed. Upon RACK-TO or RTO, the RACK policy performs a scan for RACK-
  // eligible packets.
  RetransmissionPolicyAction HandleRetransmitTimeoutEvent(
      ConnectionState* connection_state) override;

  // Returns RACK timeout for a given packet if applicable, otherwise
  // InfiniteDuration such that min timeout calculation will be unaffected.
  absl::Duration GetTimeoutOfPacket(ConnectionState* connection_state,
                                    PacketMetadata* packet_metadata) override;

  // Handler functions below are unused by RACK.
  RetransmissionPolicyAction InitConnection(
      ConnectionState* connection_state) override {
    return NoOpAction();
  }

  RetransmissionPolicyAction SetupRetransmission(
      PacketMetadata* packet_metadata,
      ConnectionState* connection_state) override {
    return NoOpAction();
  }

  RetransmissionPolicyAction HandleAckToUlp(
      PacketMetadata* packet_metadata,
      ConnectionState* connection_state) override {
    return NoOpAction();
  }

  RetransmissionPolicyAction HandlePiggybackedAck(
      const Packet* packet, ConnectionState* connection_state) override {
    return NoOpAction();
  }

  RetransmissionPolicyAction HandleImplicitAck(
      const Packet* packet, ConnectionState* connection_state) override {
    return NoOpAction();
  }

  RetransmissionPolicyAction HandleScheduledEvent(
      ConnectionState* connection_state) override {
    return NoOpAction();
  }

 private:
  // Update RACK calculations. With swift enabled, RACK calculations are
  // performed within RUE and this method is unused.
  void RackUpdate(ConnectionState* connection_state, absl::Duration tx_time);
  // Compute RACK's reorder window. With swift enabled, RACK calculations are
  // performed within RUE and this method is unused.
  absl::Duration CalculateRackWindow(ConnectionState* connection_state);

  // Iterates over request/data window metadata and apply RACK updates.
  void ApplyAckWindowMetadata(ConnectionState* connection_state,
                              TransmitterReliabilityWindowMetadata* tx_window,
                              uint32_t base_psn,
                              const FalconAckPacketBitmap& ack_bitmap);

  // Retrieves PacketMetadata, used by RACK to store received state.
  PacketMetadata* GetPacketMetadata(ConnectionState* connection_state,
                                    uint32_t rsn, falcon::PacketType type);

  // Performs RACK scan over TX windows and returns retx work IDs which meet
  // RACK criteria for retransmission.
  RetransmissionPolicyAction PerformRackScan(ConnectionState* connection_state);

  FalconModelInterface* const falcon_;

  // These two parameters are used for RACK calculation. With swift enabled,
  // RACK calculations are performed within RUE and these variables are unused.
  double rack_time_window_rtt_factor_;
  absl::Duration min_rack_time_window_;

  // If true, applies simplified RACK scan criteria. Disables RACK background
  // scans and ignores RACK RTO when setting packet retransmission timeouts.
  // Upon receiving an ACK, retransmits all packets with transmission time
  // before RACK xmit_ts.
  bool bypass_rto_check_;
  // If true, disables RACK scan range limit, allowing RACK scan to cover
  // entire TX window. Otherwise, by default, RACK scan range is limited to
  // the ACK request/data bitmap sizes.
  bool bypass_scan_range_limit_;

  // Maximum early retx attempts for a packet.
  int early_retx_threshold_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN3_RACK_RETRANSMISSION_POLICY_H_
