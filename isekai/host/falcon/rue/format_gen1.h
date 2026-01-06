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

#ifndef ISEKAI_HOST_FALCON_RUE_FORMAT_GEN1_H_
#define ISEKAI_HOST_FALCON_RUE_FORMAT_GEN1_H_

#include <bitset>
#include <cstdint>
#include <sstream>
#include <string>

#include "absl/base/optimization.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/rue/bits.h"

namespace falcon_rue {

struct __attribute__((packed)) Event_Gen1 {
  // cid
  uint32_t connection_id : kConnectionIdBits;
  // event_type
  falcon::RueEventType event_type : kEventTypeBits;
  // t1
  uint32_t timestamp_1 : kTimeBits;
  // t2
  uint32_t timestamp_2 : kTimeBits;
  // t3
  uint32_t timestamp_3 : kTimeBits;
  // t4
  uint32_t timestamp_4 : kTimeBits;
  // retx_count
  uint8_t retransmit_count : kGen1RetransmitCountBits;
  // retx_reason
  falcon::RetransmitReason retransmit_reason : kRetransmitReasonBits;
  // nack_code
  falcon::NackCode nack_code : kNackCodeBits;
  // forward_hops
  uint8_t forward_hops : kForwardHopsBits;
  // rx_req_buf_level
  uint8_t rx_buffer_level : kRxBufferLevelBits;
  // eack_own bits
  uint8_t eack_own : kEackOwnBits;
  // cc_meta
  uint32_t cc_metadata : kGen1CcMetadataBits;
  // cwnd_frac and cwnd concatenated
  uint32_t fabric_congestion_window : kFabricCongestionWindowBits;
  // ipg
  uint32_t inter_packet_gap : kInterPacketGapBits;
  // ncwnd
  uint32_t nic_congestion_window : kGen1NicCongestionWindowBits;
  // retx_to
  uint32_t retransmit_timeout : kTimeBits;
  // num_acked
  uint16_t num_packets_acked : kNumPacketsAckedBits;
  // eack bits
  uint8_t eack_drop : 1;
  uint8_t eack : 1;
  // old ECN counter
  uint16_t reserved_0 : 8;
  // event_q_sel
  uint8_t event_queue_select : kEventQueueSelectBits;
  // delay_sel
  falcon::DelaySelect delay_select : kDelaySelectBits;
  // window_guard
  uint32_t fabric_window_time_marker : kTimeBits;
  // ncwnd_guard
  uint32_t nic_window_time_marker : kTimeBits;
  // ncwnd_dir
  falcon::WindowDirection nic_window_direction : kWindowDirectionBits;
  // delay_base
  uint32_t base_delay : kTimeBits;
  // delay_state
  uint32_t delay_state : kTimeBits;
  // smoothed_rtt
  uint32_t rtt_state : kTimeBits;
  // cc_opaque
  // bit 0 being used by HW-RUE for randomizing paths
  uint8_t cc_opaque : kGen1CcOpaqueBits;
  // ecn accumulated
  uint16_t ecn_accumulated : kEcnAccumulatedBits;
  // reserved
  uint64_t reserved_1 : 64;
  uint64_t reserved_2 : 43;
  // gen
  uint8_t gen_bit : 1;

  std::string ToString() const {
    std::ostringstream stream;
    stream << "GEN1 Event: " << "\n---begin---"
           << "\n  connection_id   : " << connection_id
           << "\n  event_type      : "
           << falcon::RueEventTypeToString(event_type)
           << "\n  timestamp_1     : " << timestamp_1
           << "\n  timestamp_2     : " << timestamp_2
           << "\n  timestamp_3     : " << timestamp_3
           << "\n  timestamp_4     : " << timestamp_4
           << "\n  retx_count      : " << static_cast<int>(retransmit_count)
           << "\n  retx_reason     : "
           << falcon::RetransmitReasonToString(retransmit_reason)
           << "\n  nack_code       : " << falcon::NackCodeToString(nack_code)
           << "\n  forward_hops    : " << static_cast<int>(forward_hops)
           << "\n  rx_buffer_level : " << static_cast<int>(rx_buffer_level)
           << "\n  eack_own_bits   : " << std::bitset<kEackOwnBits>(eack_own)
           << "\n  cc_metadata     : " << cc_metadata
           << "\n  fabric_cwnd     : " << fabric_congestion_window
           << "\n  fabric_ipg      : " << inter_packet_gap
           << "\n  nic_cwnd        : " << nic_congestion_window
           << "\n  retx_timeout    : " << retransmit_timeout
           << "\n  num_acked       : " << num_packets_acked
           << "\n  eack_drop_bit   : " << static_cast<int>(eack_drop)
           << "\n  eack_bit        : " << static_cast<int>(eack)
           << "\n  reserved_0      : " << static_cast<int>(reserved_0)
           << "\n  event_q_select  : " << static_cast<int>(event_queue_select)
           << "\n  delay_select    : "
           << falcon::DelaySelectToString(delay_select)
           << "\n  fcwnd_guard     : " << fabric_window_time_marker
           << "\n  ncwnd_guard     : " << nic_window_time_marker
           << "\n  ncnwd_direction : "
           << falcon::WindowDirectionToString(nic_window_direction)
           << "\n  delay_base      : " << base_delay
           << "\n  delay_state     : " << delay_state
           << "\n  rtt_state       : " << rtt_state
           << "\n  cc_opaque       : " << static_cast<int>(cc_opaque)
           << "\n  ecn_accumulated : " << static_cast<int>(ecn_accumulated)
           << "\n  reserved_1      : " << static_cast<uint64_t>(reserved_1)
           << "\n  reserved_2      : " << static_cast<uint64_t>(reserved_2)
           << "\n  gen_bit         : " << static_cast<int>(gen_bit)
           << "\n----end----";
    return stream.str();
  }
};

struct __attribute__((packed)) Response_Gen1 {
  // cid
  uint32_t connection_id : kConnectionIdBits;
  // randomize_path
  bool randomize_path : 1;
  // eack_own bits
  uint8_t eack_own : kEackOwnBits;
  // cc_meta
  uint32_t cc_metadata : kCcMetadataBits;
  // cwnd_frac and cwnd concatenated
  uint32_t fabric_congestion_window : kFabricCongestionWindowBits;
  // ipg
  uint32_t inter_packet_gap : kInterPacketGapBits;
  // ncwnd
  uint32_t nic_congestion_window : kGen1NicCongestionWindowBits;
  // retx_to
  uint32_t retransmit_timeout : kTimeBits;
  // event_q_sel
  uint8_t event_queue_select : kEventQueueSelectBits;
  // delay_sel
  falcon::DelaySelect delay_select : kDelaySelectBits;
  // window_guard
  uint32_t fabric_window_time_marker : kTimeBits;
  // ncwnd_guard
  uint32_t nic_window_time_marker : kTimeBits;
  // ncwnd_dir
  falcon::WindowDirection nic_window_direction : kWindowDirectionBits;
  // delay_base
  uint32_t base_delay : kTimeBits;
  // delay_state
  uint32_t delay_state : kTimeBits;
  // smoothed_rtt
  uint32_t rtt_state : kTimeBits;
  // cc_opaque
  // bit 0 being used by HW-RUE for randomizing paths
  uint8_t cc_opaque : kGen1CcOpaqueBits;
  // reserve
  uint64_t padding_1 : 64;
  uint64_t padding_2 : 64;
  uint64_t padding_3 : 64;
  uint64_t padding_4 : 64;
  uint64_t padding_5 : 4;

  std::string ToString() const {
    std::ostringstream stream;
    stream << "GEN1 Response: " << "\n---begin---"
           << "\n  connection_id   : " << connection_id
           << "\n  randomize_path  : " << (randomize_path ? 1 : 0)
           << "\n  eack_own_bits   : " << std::bitset<kEackOwnBits>(eack_own)
           << "\n  cc_metadata     : " << cc_metadata
           << "\n  fabric_cwnd     : " << fabric_congestion_window
           << "\n  fabric_ipg      : " << inter_packet_gap
           << "\n  nic_cwnd        : " << nic_congestion_window
           << "\n  retx_timeout    : " << retransmit_timeout
           << "\n  event_q_select  : " << static_cast<int>(event_queue_select)
           << "\n  delay_select    : "
           << falcon::DelaySelectToString(delay_select)
           << "\n  fcwnd_guard     : " << fabric_window_time_marker
           << "\n  ncwnd_guard     : " << nic_window_time_marker
           << "\n  ncnwd_direction : "
           << falcon::WindowDirectionToString(nic_window_direction)
           << "\n  delay_base      : " << base_delay
           << "\n  delay_state     : " << delay_state
           << "\n  rtt_state       : " << rtt_state
           << "\n  cc_opaque       : " << static_cast<int>(cc_opaque)
           << "\n  padding_1       : " << static_cast<uint64_t>(padding_1)
           << "\n  padding_2       : " << static_cast<uint64_t>(padding_2)
           << "\n  padding_3       : " << static_cast<uint64_t>(padding_3)
           << "\n  padding_4       : " << static_cast<uint64_t>(padding_4)
           << "\n  padding_5       : " << static_cast<uint64_t>(padding_5)
           << "\n----end----";
    return stream.str();
  }
};

static_assert(sizeof(Event_Gen1) == ABSL_CACHELINE_SIZE,
              "Event is not one cache line :(");
static_assert(sizeof(Response_Gen1) == ABSL_CACHELINE_SIZE,
              "Response is not one cache line :(");

}  // namespace falcon_rue

#endif  // ISEKAI_HOST_FALCON_RUE_FORMAT_GEN1_H_
