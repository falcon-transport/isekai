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

#include "isekai/common/default_config_generator.h"

#include <cstdint>
#include <limits>

#include "absl/log/log.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/constants.h"
#include "isekai/host/falcon/rue/algorithm/swift.h"

namespace isekai {

FalconConfig DefaultConfigGenerator::Gen1Falcon::DefaultConfig() {
  FalconConfig config;

  // Number of cache entries.
  config.set_connection_context_cache_size(10000);
  // Generally a power of 2.
  config.set_threshold_solicit(8192);
  config.set_simulation_mode(FalconConfig::PROTOCOL);
  // Used by the connection state manager.
  config.set_lookup_delay_ns(0);
  config.set_tx_buffer_minimum_allocation_unit(1);
  config.set_rx_buffer_minimum_allocation_unit(1);
  config.set_resource_reservation_mode(FalconConfig::BYPASS_RESERVATION);
  config.mutable_resource_credits()->CopyFrom(DefaultResourceCredits());
  config.mutable_ulp_xoff_thresholds()->CopyFrom(DefaultUlpXoffThresholds());
  FalconConfig::FalconNetworkRequestsOccupancyThresholds
      network_request_occupancy_thresholds =
          DefaultFalconNetworkRequestsOccupancyThresholds();
  config.mutable_falcon_network_requests_rx_buffer_pool_thresholds()->CopyFrom(
      network_request_occupancy_thresholds);
  config.mutable_falcon_network_requests_rx_packet_pool_thresholds()->CopyFrom(
      network_request_occupancy_thresholds);
  config.mutable_falcon_network_requests_tx_packet_pool_thresholds()->CopyFrom(
      network_request_occupancy_thresholds);
  config.mutable_ema_coefficients()->CopyFrom(
      DefaultTargetBufferOccupancyEmaCoefficients());
  FalconConfig::TargetBufferOccupancyQuantizationTables::QuantizationTable
      quantization_table = DefaultQuantizationTable();
  config.mutable_quantization_tables()->mutable_tx_context()->CopyFrom(
      quantization_table);
  config.mutable_quantization_tables()->mutable_rx_context()->CopyFrom(
      quantization_table);
  config.mutable_quantization_tables()->mutable_rx_buffer()->CopyFrom(
      quantization_table);
  //
  // time. This must be non-zero, because a 0 tick-time will schedule itself
  // forever without making progress if there is a retransmission packet
  // with a timer expiring in the future.
  config.set_falcon_tick_time_ns(kFalconTickTimeNs);
  config.mutable_connection_scheduler_policies()->CopyFrom(
      DefaultConnectionSchedulerPolicies());

  config.set_inter_connection_retransmission_scheduling_policy(
      FalconConfig::ROUND_ROBIN);
  config.set_intra_connection_retransmission_scheduling_policy(
      FalconConfig::ROUND_ROBIN);
  // In real HW, the packets from connection / retransmission scheduler and
  // Acks/Nacks are arbitrated in a round-robin manner. We model it by using
  // a weighted round robin arbiter and assign connection scheduler,
  // retransmission scheduler and Ack/Nack scheduler with weights 1, 1, 2,
  // respectively.
  config.set_connection_scheduler_weight(1);
  config.set_retransmission_scheduler_weight(1);
  config.set_ack_nack_scheduler_weight(2);
  config.set_admission_control_policy(FalconConfig::RX_WINDOW_BASED);
  config.set_admission_window_bytes(std::numeric_limits<int>::max());

  // Default to Falcon v1.
  config.set_version(1);
  config.mutable_early_retx()->CopyFrom(DefaultEarlyRetx());
  // In Gen_1, EACK is based on hole in rx bitmap.
  config.set_eack_trigger_by_hole_in_rx_bitmap(true);
  // Disable reflection of receiver buffer occupancy until we want ncwnd
  // modulation to be always turned ON.
  //
  // always.
  config.set_enable_rx_buffer_occupancy_reflection(false);
  config.mutable_rue()->CopyFrom(DefaultRue());
  // Enable AR-bit by default.
  config.set_enable_ack_request_bit(true);
  // fcwnd <=2 sets AR bit on every packet.
  config.set_ack_request_fcwnd_threshold(2);
  // fcwnd > 2 sets AR bit with 10% probability.
  config.set_ack_request_percent(10);
  config.mutable_ack_coalescing_thresholds()->set_count(10);
  config.mutable_ack_coalescing_thresholds()->set_timeout_ns(8000);
  config.mutable_op_boundary_ar_bit()->set_enable(false);
  // 15M.
  config.mutable_op_boundary_ar_bit()->set_acks_per_sec(15000000);
  // Max number of active connections.
  config.mutable_op_boundary_ar_bit()->set_ack_burst_size(10000);
  config.mutable_op_boundary_ar_bit()->set_ack_refill_interval_ns(200);
  config.set_inter_host_rx_scheduling_policy(FalconConfig::ROUND_ROBIN);
  config.set_inter_host_rx_scheduling_tick_ns(5);
  config.set_rx_falcon_ulp_link_gbps(200.0);

  return config;
}

FalconConfig::ResourceCredits
DefaultConfigGenerator::Gen1Falcon::DefaultResourceCredits() {
  FalconConfig::ResourceCredits falcon_credits;
  // TX packet credits.
  falcon_credits.mutable_tx_packet_credits()->set_ulp_requests(kMaxCredits);
  falcon_credits.mutable_tx_packet_credits()->set_ulp_data(kMaxCredits);
  falcon_credits.mutable_tx_packet_credits()->set_network_requests(kMaxCredits);
  // TX buffer credits.
  falcon_credits.mutable_tx_buffer_credits()->set_ulp_requests(kMaxCredits);
  falcon_credits.mutable_tx_buffer_credits()->set_ulp_data(kMaxCredits);
  falcon_credits.mutable_tx_buffer_credits()->set_network_requests(kMaxCredits);
  // RX packet credits.
  falcon_credits.mutable_rx_packet_credits()->set_ulp_requests(kMaxCredits);
  falcon_credits.mutable_rx_packet_credits()->set_network_requests(kMaxCredits);
  // RX buffer credits.
  falcon_credits.mutable_rx_buffer_credits()->set_ulp_requests(kMaxCredits);
  falcon_credits.mutable_rx_buffer_credits()->set_network_requests(kMaxCredits);
  return falcon_credits;
}

// Setting them to 0 effectively means request Xoff will never be
// asserted.
FalconConfig::UlpXoffThresholds
DefaultConfigGenerator::Gen1Falcon::DefaultUlpXoffThresholds() {
  FalconConfig::UlpXoffThresholds falcon_thresholds;
  falcon_thresholds.set_tx_packet_request(0);
  falcon_thresholds.set_tx_buffer_request(0);
  falcon_thresholds.set_tx_packet_data(0);
  falcon_thresholds.set_tx_buffer_data(0);
  falcon_thresholds.set_rx_packet_request(0);
  falcon_thresholds.set_rx_buffer_request(0);
  return falcon_thresholds;
}

// Setting them to INT32_MAX effectively means HoL network requests will
// never be prioritized.
FalconConfig::FalconNetworkRequestsOccupancyThresholds DefaultConfigGenerator::
    Gen1Falcon::DefaultFalconNetworkRequestsOccupancyThresholds() {
  FalconConfig::FalconNetworkRequestsOccupancyThresholds falcon_thresholds;
  falcon_thresholds.set_green_zone_end(kMaxCredits);
  falcon_thresholds.set_yellow_zone_end(kMaxCredits);
  return falcon_thresholds;
}

// Sets the EMA coefficients such that equal weight is given to current
// occupancy and previous occupancy.
FalconConfig::TargetBufferOccupancyEmaCoefficients DefaultConfigGenerator::
    Gen1Falcon::DefaultTargetBufferOccupancyEmaCoefficients() {
  FalconConfig::TargetBufferOccupancyEmaCoefficients ema_coefficients;
  ema_coefficients.set_tx_context(1);
  ema_coefficients.set_rx_context(1);
  ema_coefficients.set_rx_buffer(1);
  return ema_coefficients;
}

FalconConfig::TargetBufferOccupancyQuantizationTables::QuantizationTable
DefaultConfigGenerator::Gen1Falcon::DefaultQuantizationTable() {
  FalconConfig::TargetBufferOccupancyQuantizationTables::QuantizationTable
      quantization_table;
  quantization_table.set_quantization_level_0_threshold(2048);
  quantization_table.set_quantization_level_1_threshold(4096);
  quantization_table.set_quantization_level_2_threshold(6144);
  quantization_table.set_quantization_level_3_threshold(8192);
  quantization_table.set_quantization_level_4_threshold(10240);
  quantization_table.set_quantization_level_5_threshold(12288);
  quantization_table.set_quantization_level_6_threshold(14336);
  quantization_table.set_quantization_level_7_threshold(16384);
  quantization_table.set_quantization_level_8_threshold(18432);
  quantization_table.set_quantization_level_9_threshold(20480);
  quantization_table.set_quantization_level_10_threshold(22528);
  quantization_table.set_quantization_level_11_threshold(24576);
  quantization_table.set_quantization_level_12_threshold(26624);
  quantization_table.set_quantization_level_13_threshold(28672);
  quantization_table.set_quantization_level_14_threshold(30720);
  quantization_table.set_quantization_level_15_threshold(32768);
  quantization_table.set_quantization_level_16_threshold(34816);
  quantization_table.set_quantization_level_17_threshold(36864);
  quantization_table.set_quantization_level_18_threshold(38912);
  quantization_table.set_quantization_level_19_threshold(40960);
  quantization_table.set_quantization_level_20_threshold(43008);
  quantization_table.set_quantization_level_21_threshold(45056);
  quantization_table.set_quantization_level_22_threshold(47104);
  quantization_table.set_quantization_level_23_threshold(49152);
  quantization_table.set_quantization_level_24_threshold(51200);
  quantization_table.set_quantization_level_25_threshold(53248);
  quantization_table.set_quantization_level_26_threshold(55296);
  quantization_table.set_quantization_level_27_threshold(57344);
  quantization_table.set_quantization_level_28_threshold(59392);
  quantization_table.set_quantization_level_29_threshold(61440);
  quantization_table.set_quantization_level_30_threshold(63488);
  return quantization_table;
}

FalconConfig::ConnectionSchedulerPolicies
DefaultConfigGenerator::Gen1Falcon::DefaultConnectionSchedulerPolicies() {
  FalconConfig::ConnectionSchedulerPolicies
      default_connection_scheduler_policies;
  default_connection_scheduler_policies.set_inter_packet_type_scheduling_policy(
      FalconConfig::ROUND_ROBIN);
  default_connection_scheduler_policies.set_intra_packet_type_scheduling_policy(
      FalconConfig::ROUND_ROBIN);
  return default_connection_scheduler_policies;
}

FalconConfig::EarlyRetx DefaultConfigGenerator::Gen1Falcon::DefaultEarlyRetx() {
  FalconConfig::EarlyRetx early_retx;
  early_retx.set_ooo_count_threshold(3);
  early_retx.set_enable_ooo_count(false);
  early_retx.set_ooo_distance_threshold(3);
  early_retx.set_enable_ooo_distance(true);
  early_retx.set_enable_eack_own(true);
  FalconConfig::EarlyRetx::EackOwnMetadata* eack_own_metadata =
      early_retx.mutable_eack_own_metadata();
  eack_own_metadata->set_enable_recency_check_bypass(true);
  eack_own_metadata->set_enable_scanning_exit_criteria_bypass(true);
  eack_own_metadata->set_enable_smaller_psn_recency_check_bypass(true);
  eack_own_metadata->set_ignore_incoming_ar_bit(true);
  eack_own_metadata->set_enable_pause_initial_transmission_on_oow_drops(true);
  eack_own_metadata->set_request_window_slack(32);
  eack_own_metadata->set_data_window_slack(64);
  early_retx.set_early_retx_threshold(16);
  return early_retx;
}

FalconConfig::Rue DefaultConfigGenerator::Gen1Falcon::DefaultRue() {
  FalconConfig::Rue rue;

  //  Falcon latency in creating the event and reading the response.
  rue.set_falcon_latency_ns(234);
  // Falcon unit time, in hardware it is 131072ps.
  rue.set_falcon_unit_time_ns(131.072);
  // TW unit time, in hardware it is 512.
  rue.set_tw_unit_time_ns(512);
  // Initial fcwnd = 32.
  rue.set_initial_fcwnd(32);
  // Initial ncwnd = 64.
  rue.set_initial_ncwnd(64);
  // Initial retransmit timeout = 1ms.
  rue.set_initial_retransmit_timeout_ns(1000000);
  // Fabric RTT.
  rue.set_delay_select(FalconConfig::Rue::FABRIC);
  // 25us base target delay.
  rue.set_base_delay_us(25);
  // Event mailbox size.
  rue.set_event_queue_size(65536);
  // Event queue threshold 1.
  rue.set_event_queue_threshold_1(65536);
  // Event queue threshold 2.
  rue.set_event_queue_threshold_2(65536);
  // Event queue threshold 3.
  rue.set_event_queue_threshold_3(65536);
  // 1024 Falcon time units, converted to ns.
  rue.set_predicate_1_time_threshold_ns(131 * 1024);
  // 512 ACK packets between events.
  rue.set_predicate_2_packet_count_threshold(512);
  // Default fixed latency model representing RUE processing speed of 66ns
  // -> 15.1515M events/sec.
  rue.mutable_fixed_latency_model()->set_latency_ns(66);

  rue.set_algorithm("swift");
  auto swift = rue.mutable_swift();
  // Generate default Swift configuration.
  auto swift_config = isekai::rue::SwiftGen1::DefaultConfiguration();
  swift->set_randomize_path(swift_config.randomize_path());
  swift->set_plb_target_rtt_multiplier(
      swift_config.plb_target_rtt_multiplier());
  swift->set_plb_congestion_threshold(swift_config.plb_congestion_threshold());
  swift->set_plb_attempt_threshold(swift_config.plb_attempt_threshold());
  swift->set_target_rx_buffer_level(swift_config.target_rx_buffer_level());
  swift->set_max_flow_scaling(swift_config.max_flow_scaling());
  swift->set_max_decrease_on_eack_nack_drop(
      swift_config.max_decrease_on_eack_nack_drop());
  swift->set_max_fcwnd(swift_config.max_fabric_congestion_window());
  swift->set_max_ncwnd(swift_config.max_nic_congestion_window());
  swift->set_fabric_additive_increment_factor(
      swift_config.fabric_additive_increment_factor());

  return rue;
}

FalconConfig DefaultConfigGenerator::Gen2Falcon::DefaultConfig() {
  FalconConfig config = Gen1Falcon::DefaultConfig();
  config.set_version(2);
  config.mutable_gen2_config_options()
      ->mutable_multipath_config()
      ->set_path_selection_policy(
          FalconConfig::Gen2ConfigOptions::MultipathConfig::
              OPEN_LOOP_FCWND_WEIGHTED_ROUND_ROBIN);
  config.mutable_gen2_config_options()
      ->mutable_multipath_config()
      ->set_single_path_connection_accept_stale_acks(false);
  config.mutable_gen2_config_options()
      ->mutable_multipath_config()
      ->set_multipath_connection_accept_stale_acks(false);
  config.mutable_gen2_config_options()
      ->mutable_multipath_config()
      ->set_batched_packet_scheduling(false);
  config.set_rx_falcon_ulp_link_gbps(400.0);
  config.mutable_gen2_config_options()
      ->mutable_multipath_config()
      ->set_retx_flow_label(FalconConfig::Gen2ConfigOptions::MultipathConfig::
                                SAME_FLOW_ID_AS_INITIAL_TX);
  config.mutable_gen2_config_options()
      ->mutable_per_connection_ulp_backpressure()
      ->CopyFrom(DefaultPerConnectionBackpressure());
  config.mutable_gen2_config_options()->set_decrement_orc_on_pull_response(
      false);
  return config;
}

FalconConfig::PerConnectionBackpressure
DefaultConfigGenerator::Gen2Falcon::DefaultPerConnectionBackpressure() {
  FalconConfig::PerConnectionBackpressure per_connection_backpressure;
  per_connection_backpressure.set_enable_backpressure(false);
  return per_connection_backpressure;
}

FalconConfig DefaultConfigGenerator::Gen3Falcon::DefaultConfig() {
  FalconConfig config = Gen2Falcon::DefaultConfig();
  config.set_version(3);
  auto early_retx = config.mutable_early_retx();
  //
  early_retx->set_enable_rack(false);
  // RACK needs an extra time window to decide drop vs. OOO. The size of the
  // time window is set as a fraction of RTT. 0.25 is consistent with TCP
  // RACK [RFC 8985].
  early_retx->set_rack_time_window_rtt_factor(0.25);
  // By default there is no minimum for the reorder window.
  early_retx->set_min_rack_time_window_ns(0);
  // RACK RTO retransmission condition applies and background scans are enabled.
  early_retx->set_rack_bypass_rto_check(false);
  // RACK scan range is limited to the request/data ACK bitmap sizes.
  early_retx->set_rack_bypass_scan_range_limit(false);
  // TLP is disabled by default.
  early_retx->set_enable_tlp(false);
  // By default setting the minimum value for TLP timeout to 2x target
  // delay.
  early_retx->set_min_tlp_timeout_ns(50000);
  // This is consistant with TCP TLP [RFC 8985].
  early_retx->set_tlp_timeout_rtt_factor(2.0);
  // By default TLP sends the first unreceived packet as probe.
  early_retx->set_tlp_type(FalconConfig::EarlyRetx::FIRST_UNRECEIVED);
  // TLP probe selection scan is limited to the request/data ACK bitmap sizes.
  early_retx->set_tlp_bypass_scan_range_limit(false);
  // TLP sends at most one probe per timeout across request/data windows.
  early_retx->set_tlp_per_window_probe(false);
  // Default RX window size is 256 (same as Gen2).
  config.mutable_gen3_config_options()->set_rx_window_size(256);
  return config;
}

FalconConfig DefaultConfigGenerator::DefaultFalconConfig(int version) {
  switch (version) {
    case 1:
      return Gen1Falcon::DefaultConfig();
    case 2:
      return Gen2Falcon::DefaultConfig();
    case 3:
      return Gen3Falcon::DefaultConfig();
    default:
      LOG(FATAL) << "Unsupported version type: " << version;
  }
}

RdmaConfig DefaultConfigGenerator::DefaultRdmaConfig() {
  RdmaConfig config;
  constexpr int32_t kMaxInt32 = std::numeric_limits<int32_t>::max();
  config.set_mtu(kDefaultRdmaMtu);
  config.set_max_segment_length((static_cast<uint32_t>(1) << 31) - 1);
  config.set_max_inline_payload_length(224);
  config.set_work_scheduler_quanta(4096);
  config.set_chip_cycle_time_ns(10);
  // Global Falcon credits available to RDMA.
  config.mutable_global_credits()->set_tx_packet_request(kMaxInt32);
  config.mutable_global_credits()->set_tx_buffer_request(kMaxInt32);
  config.mutable_global_credits()->set_rx_packet_request(kMaxInt32);
  config.mutable_global_credits()->set_rx_buffer_request(kMaxInt32);
  config.mutable_global_credits()->set_tx_packet_data(kMaxInt32);
  config.mutable_global_credits()->set_tx_buffer_data(kMaxInt32);
  // Falcon credits available to each RDMA QP.
  config.mutable_per_qp_credits()->set_tx_packet_request(kMaxInt32);
  config.mutable_per_qp_credits()->set_tx_buffer_request(kMaxInt32);
  config.mutable_per_qp_credits()->set_rx_packet_request(kMaxInt32);
  config.mutable_per_qp_credits()->set_rx_buffer_request(kMaxInt32);
  config.mutable_per_qp_credits()->set_tx_packet_data(kMaxInt32);
  config.mutable_per_qp_credits()->set_tx_buffer_data(kMaxInt32);
  config.set_scheduler_pipeline_delay_in_cycles(0);
  config.mutable_tx_rate_limiter()->set_refill_interval_ns(500);
  config.mutable_tx_rate_limiter()->set_burst_size_bytes(12500);
  config.set_inbound_read_queue_depth(std::numeric_limits<int32_t>::max());
  config.set_outbound_read_queue_depth(std::numeric_limits<int32_t>::max());
  config.set_rnr_timeout_us(0);
  config.set_write_random_rnr_probability(0);
  config.set_read_random_rnr_probability(0);
  // By default, consider only one bifurcated host attached to this RNIC. The
  // RDMA block has total 500kB memory that gets distributed across all the
  // available hosts. In this case, all of the 500kB memory is allocated to
  // one hosts.
  config.mutable_rx_buffer_config()->add_buffer_size_bytes(512 * 1024);
  // Disable the PCIe delay for completion messages.
  config.set_enable_pcie_delay_for_completion(false);
  config.set_ack_nack_latency_ns(0);
  config.set_response_latency_ns(0);
  config.set_max_qp_oprate_million_per_sec(40);
  return config;
}

RNicConfig DefaultConfigGenerator::DefaultRNicConfig() {
  RNicConfig config;
  config.add_host_interface_config()->CopyFrom(DefaultHostInterfaceConfig());
  return config;
}

MemoryInterfaceConfig DefaultConfigGenerator::DefaultHostInterfaceConfig() {
  MemoryInterfaceConfig config;
  // Gen1 has PCIe v4 which has max unidirectional bandwidth as 256 gbps,
  // 220 gbps when removing the transaction overhead). Assuming no bifurcation
  // as default, we set PCIe bandwidth to be 220 gbps.
  config.mutable_write_queue_config()->set_bandwidth_bps(220e9);
  config.mutable_write_queue_config()->set_delay_distribution(
      MemoryInterfaceConfig_MemoryDelayDistribution_CONST);
  config.mutable_write_queue_config()->set_memory_delay_const_ns(0);
  config.mutable_write_queue_config()->set_memory_interface_queue_size_packets(
      1);
  return config;
}

// Options for the Traffic Shaper model.
TrafficShaperConfig DefaultConfigGenerator::DefaultTrafficShaperConfig() {
  TrafficShaperConfig config;
  config.set_timing_wheel_slots(kDefaultTimingWheelSlots);
  config.set_slot_granularity_ns(2048);
  return config;
}

StatisticsCollectionConfig::FalconFlags
DefaultConfigGenerator::DefaultFalconStatsFlags() {
  StatisticsCollectionConfig::FalconFlags config;
  config.set_enable_vector_scheduler_lengths(false);
  config.set_enable_histogram_scheduler_lengths(false);
  config.set_enable_rue_cc_metrics(false);
  config.set_enable_rue_event_queue_length(false);
  config.set_enable_xoff_timelines(false);
  config.set_enable_max_retransmissions(false);
  config.set_enable_per_connection_rdma_counters(true);
  config.set_enable_per_connection_network_counters(true);
  config.set_enable_per_connection_ack_nack_counters(true);
  config.set_enable_per_connection_initiator_txn_counters(true);
  config.set_enable_per_connection_target_txn_counters(true);
  config.set_enable_per_connection_rue_counters(true);
  config.set_enable_per_connection_rue_drop_counters(true);
  config.set_enable_per_connection_ack_reason_counters(false);
  config.set_enable_per_connection_packet_drop_counters(false);
  config.set_enable_per_connection_retx_counters(true);
  config.set_enable_solicitation_counters(false);
  config.set_enable_per_connection_resource_credit_counters(false);
  config.set_enable_per_connection_cwnd_pause(false);
  config.set_enable_per_connection_max_rsn_difference(false);
  config.set_enable_per_connection_scheduler_queue_length(false);
  config.set_enable_per_connection_scheduler_queue_length_histogram(false);
  config.set_enable_per_connection_backpressure_alpha_carving_limits(false);
  config.set_enable_per_connection_window_usage(false);
  config.set_enable_per_connection_initial_tx_rsn_timeline(false);
  config.set_enable_per_connection_rx_from_ulp_rsn_timeline(false);
  config.set_enable_per_connection_retx_rsn_timeline(false);
  config.set_enable_per_connection_rsn_receive_timeline(false);
  config.set_enable_connection_scheduler_max_delayed_packet_stats(false);
  config.set_enable_resource_manager_ema_occupancy(false);
  config.set_enable_global_resource_credits_timeline(false);
  config.set_enable_inter_host_rx_scheduler_queue_length(false);
  return config;
}

StatisticsCollectionConfig::PacketBuilderFlags
DefaultConfigGenerator::DefaultPacketBuilderStatsFlags() {
  StatisticsCollectionConfig::PacketBuilderFlags config;
  config.set_enable_scalar_packet_delay(true);
  config.set_enable_vector_packet_delay(false);
  config.set_enable_queue_length(false);
  config.set_enable_discard_and_drops(true);
  config.set_enable_scalar_tx_rx_packets_bytes(true);
  config.set_enable_vector_tx_rx_bytes(false);
  config.set_enable_pfc(false);
  config.set_enable_xoff_duration(false);
  return config;
}

StatisticsCollectionConfig::RdmaFlags
DefaultConfigGenerator::DefaultRdmaStatsFlags() {
  StatisticsCollectionConfig::RdmaFlags config;
  config.set_enable_op_timeseries(false);
  config.set_enable_per_qp_xoff(false);
  config.set_enable_total_xoff(false);
  config.set_enable_credit_stall(false);
  config.set_enable_histograms(false);
  return config;
}

StatisticsCollectionConfig::RouterFlags
DefaultConfigGenerator::DefaultRouterStatsFlags() {
  StatisticsCollectionConfig::RouterFlags config;
  config.set_enable_port_stats_collection(false);
  config.set_port_stats_collection_interval_us(5);
  config.set_enable_scalar_per_port_tx_rx_packets(false);
  config.set_enable_vector_per_port_tx_rx_bytes(false);
  config.set_enable_per_port_ingress_discards(false);
  config.set_enable_packet_discards(true);
  config.set_enable_per_port_per_queue_stats(false);
  config.set_enable_pfc_stats(false);
  return config;
}

StatisticsCollectionConfig::TrafficGeneratorFlags
DefaultConfigGenerator::DefaultTrafficGeneratorStatsFlags() {
  StatisticsCollectionConfig::TrafficGeneratorFlags config;
  config.set_enable_scalar_offered_load(true);
  config.set_enable_vector_offered_load(false);
  config.set_enable_op_schedule_interval(true);
  config.set_enable_scalar_op_stats(true);
  config.set_enable_vector_op_stats(false);
  config.set_enable_per_qp_tx_rx_bytes(false);
  return config;
}

}  // namespace isekai
