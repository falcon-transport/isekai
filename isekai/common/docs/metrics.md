# Isekai Metrics

This doc summarizes the stats recorded by each module during Isekai simulations,
along with the flags which enable them.

## Host Stats

The Falcon host contains five components, namely traffic generator, RDMA,
Falcon, traffic shaper and packet builder.

### Traffic Generator

- Flag: `enable_scalar_offered_load`
  - [scalar] `traffic_generator.offered_load_scalar.src_$0.dst_$1`
    - Unit: Gigabits
    - Reducer: Sum
    - Offered load between a source and destination host pair

- Flag: `enable_vector_offered_load`
  - [vector] `traffic_generator.offered_load_vector.src_$0.dst_$1`
    - Unit: Gigabits
    - Accumulative: Yes
    - Offered load between a source and destination host pair

- Flag: `enable_op_schedule_interval`
  - [scalar] `traffic_generator.op_schedule_interval`
    - Unit: Seconds
    - Reducer: Mean
    - Average inter op scheduling interval

- Flag: `enable_scalar_op_stats`
  - [scalar] `traffic_generator.goodput.src_$0.dst_$1`
    - Unit: Bytes
    - Reducer: Sum
    - Number of bytes successfully transferred from application
  - [scalar] `traffic_generator.completed_ops_scalar.src_$0.dst_$1`
    - Unit: # ops
    - Reducer: Sum
    - Number of ops completed
  - [scalar] `traffic_generator.op_latency_scalar.src_$0.dst_$1.pattern_$2.opcode_$3.size_$4`
    - Unit: Second
    - Reducer: Mean
    - Average op latency

- Flag: `enable_vector_op_stats`
  - [vector] `traffic_generator.generated_op_size.pattern_$0.opcode_$1`
    - Unit: Gigabits
    - Accumulative: Yes
    - Generated op size
  - [vector] `traffic_generator.completed_op_size.src_$0.dst_$1.pattern_$2.opcode_$3.size_$4`
    - Unit: bits
    - Accumulative: No
    - Completed op size
  - [vector] `traffic_generator.completed_ops.src_$0.dst_$1.pattern_$2.opcode_$3.size_$4`
    - Unit: # ops
    - Accumulative: Yes
    - Number of completed ops
  - [vector] `traffic_generator.op_latency.src_$0.dst_$1.pattern_$2.opcode_$3.size_$4`
    - Unit: Seconds
    - Accumulative: No
    - Op latency

- Flag: `enable_per_qp_tx_rx_bytes`
  - [vector] `traffic_generator.tx_bytes.local_qp_$0.remote_qp_$1`
    - Unit: Bytes
    - Accumulative: No
    - Tx bytes between a local and remote qp
  - [vector] `traffic_generator.rx_bytes.local_qp_$0.remote_qp_$1`
    - Unit: Bytes
    - Accumulative: No
    - Rx bytes between a local and remote qp

### RDMA

- Flag: `enable_op_timeseries`
  - [vector] `rdma.op_length.qp$0.cid$1.op$2`
    - Unit: Bytes
    - Accumulative: No
    - Op length
  - [vector] `rdma.op_start_rsn.qp$0.cid$1.op$2`
    -  Unit: Sequence number
    -  Accumulative: No
    -  First request sequence number (RSN) of the op
  - [vector] `rdma.op_end_rsn.qp$0.cid$1.op$2`
    - Unit: Sequence number
    - Accumulative: No
    - Last request sequence number (RSN) of the op
  - [vector] `rdma.op_post_timestamp_ns.qp$0.cid$1.op$2`
    - Unit: Nanoseconds (in simulation clock)
    - Accumulative: No
    - Op post timestamp
  - [vector] `rdma.op_start_timestamp_ns.qp$0.cid$1.op$2`
    - Unit: Nanoseconds (in simulation clock)
    - Accumulative: No
    - Op start timestamp
  - [vector] `rdma.op_finish_timestamp_ns.qp$0.cid$1.op$2`
    - Unit: Nanoseconds (in simulation clock)
    - Accumulative: No
    - Op finish timestamp when the last packet was sent to downstream
  - [vector] `rdma.op_completion_timestamp_ns.qp$0.cid$1.op$2`
    - Unit: Nanoseconds (in simulation clock)
    - Accumulative: No
    - Op completion timestamp when the last completion/data was received.
  - [vector] `rdma.total_latency_ns.qp$0.cid$1.op$2`
    - Unit: Nanoseconds
    - Accumulative: No
    - Op total latency
  - [vector] `rdma.transport_latency_ns.qp$0.cid$1.op$2`
    - Unit: Nanoseconds
    - Accumulative: No
    - Op transport latency

- Flag: `enable_per_qp_xoff`
  - [scalar] `rdma.qp$0.cid$1.request_xoff_time_ns`
    - Unit: Nanoseconds
    - Reducer: Sum
    - Total time the qp was request xoff'ed by Falcon
  - [scalar] `rdma.qp$0.cid$1.response_xoff_time_ns`
    - Unit: Nanoseconds
    - Reducer: Sum
    - Total time the qp was response xoff'ed by Falcon

- Flag: `enable_total_xoff`
  - [scalar] `rdma.total_request_xoff_time_ns`
    - Unit: Nanoseconds
    - Reducer: Sum
    - Total time RDMA was request xoff'ed by Falcon
  - [scalar] `rdma.total_global_xoff_time_ns`
    - Unit: Nanoseconds
    - Reducer: Sum
    - Total time RDMA was global xoff'ed by Falcon

- Flag: `enable_credit_stall`
  - [scalar] `rdma.total_request_credit_stall_time_ns`
    - Unit: Nanoseconds
    - Reducer: Sum
    - Total time RDMA was stalled due to insufficient request credits
  - [scalar] `rdma.total_response_credit_stall_time_ns`
    - Unit: Nanoseconds
    - Reducer: Sum
    - Total time RDMA was stalled due to insufficient response credits

### Falcon

- Flag: `enable_vector_scheduler_lengths`
  - [vector] `falcon.counter.connection_scheduler_outstanding_packets`
    - Unit: # packets
    - Accumulative: No
    - Total queue length of the connection scheduler
  - [vector] `falcon.counter.retransmission_scheduler_outstanding_packets`
    - Unit: # packets
    - Accumulative: No
    - Total queue length of the retransmission scheduler
  - [vector] `falcon.counter.ack_scheduler_outstanding_packets`
    - Unit: # packets
    - Accumulative: No
    - Total queue length of the ack/nack scheduler

- Flag: `enable_rue_cc_metrics`
  - [vector] `falcon.rue.fabric_rtt_us.cid$0`
    - Unit: Microseconds
    - Accumulative: No
    - Fabric RTT of the given connection
  - [vector] `falcon.rue.forward_owd_us.cid$0`
    - Unit: Microseconds
    - Accumulative: No
    - Forward one way delay of the given connection
  - [vector] `falcon.rue.reverse_owd_us.cid$0`
    - Unit: Microseconds
    - Accumulative: No
    - Reverse one way delay of the given connection
  - [vector] `falcon.rue.total_rtt_us.cid$0`
    - Unit: Microseconds
    - Accumulative: No
    - Total RTT of the given connection
  - [vector] `falcon.rue.rx_buffer_level.cid$0`
    - Unit: Buffer level
    - Accumulative: No
    - Current RX buffer level reported by the target
  - [vector] `falcon.rue.retransmit_timeout_us.cid$0`
    - Unit: Microseconds
    - Accumulative: No
    - Retransmit timeout for the given connection
  - [vector] `falcon.rue.fabric_cwnd.cid$0`
    - Unit: Congestion window (number of packets)
    - Accumulative: No
    - Fabric congestion window for the given connection
  - [vector] `falcon.rue.nic_cwnd.cid$0`
    - Unit: Congestion window (number of packets)
    - Accumulative: No
    - NIC congestion window for the given connection
  - [vector] `falcon.rue.plb_reroute_count.cid$0`
    - Unit: # PLB events
    - Accumulative: Yes
    - PLB reroute count for the given connection
  - [vector] `falcon.rue.num_acked_count.cid$0`
    - Unit: # packets
    - Accumulative: No
    - Number of packets acked in an ACK/NACK event for the given connection
  - (Supported from Gen2) [vector] `falcon.rue.fabric_rtt_us.cid$0.flowId$1`
    - Unit: Microseconds
    - Accumulative: No
    - Fabric RTT of the given (connection, flow)
  - (Supported from Gen2) [vector] `falcon.rue.forward_owd_us.cid$0.flowId$1`
    - Unit: Microseconds
    - Accumulative: No
    - Forward one way delay of the given (connection, flow)
  - (Supported from Gen2) [vector] `falcon.rue.reverse_owd_us.cid$0.flowId$1`
    - Unit: Microseconds
    - Accumulative: No
    - Reverse one way delay of the given (connection, flow)
  - (Supported from Gen2) [vector] `falcon.rue.total_rtt_us.cid$0.flowId$1`
    - Unit: Microseconds
    - Accumulative: No
    - Total RTT of the given (connection, flow)
  - (Supported from Gen2) [vector] `falcon.rue.reroute_count.cid$0.flowId$1`
    - Unit: # PLB events
    - Accumulative: No
    - Path selection reroute count for the given (connection, flow)
  - (Supported from Gen2) [vector] `falcon.rue.num_acked_count.cid$0.flowId$1`
    - Unit: # packets
    - Accumulative: No
    - Number of packets acked in an ACK/NACK event for the given (connection, flow)
  - (Supported from Gen2) [vector] `falcon.rue.flow_weight.cid$0.flowId$1`
    - Unit: Unitless (weight)
    - Accumulative: No
    - The weight set by the RUE in a Response for the given (connection, flow)

- Flag: `enable_rue_event_queue_length`
  - [vector] `falcon.rue.event_queue_length`
    - Unit: # RUE events
    - Accumulative: No
    - Length of the RUE Event queue

- Flag: `enable_xoff_timelines`
  - [vector] `falcon.counter.xoff_packet_builder_to_falcon`
    - Unit: Binary (Xon: 0, Xoff: 1)
    - Accumulative: No
    - Xoff from packet builder to Falcon

  - [vector] `falcon.counter.xoff_rdma_to_falcon.host$0`
    - Unit: Binary (Xon: 0, Xoff: 1)
    - Accumulative: No
    - Xoff from RDMA to Falcon at a per bifurcated host level

- Flag: `enable_max_retransmissions`
  - [scalar] `falcon.counter.max_transmission_attempts`
    - Unit: # transmission attempts
    - Reducer: Max
    - The maximum number of retransmission attempts for a given packet

#### Per-connection Statistics

- Flag: `enable_per_connection_rdma_counters`
  - [scalar] `falcon.cid$0.counter.pull_request_from_ulp`
    - Unit: # requests
    - Reducer: Sum
    - Number of pull requests from RDMA at the initiator
  - [scalar] `falcon.cid$0.counter.push_request_from_ulp`
    - Unit: # requests
    - Reducer: Sum
    - Number of push requests from RDMA at the initiator
  - [scalar] `falcon.cid$0.counter.pull_response_to_ulp`
    - Unit: # responses
    - Reducer: Sum
    - Number of pull responses to RDMA at the initiator
  - [scalar] `falcon.cid$0.counter.completions_to_ulp`
    - Unit: # completions
    - Reducer: Sum
    - Number of completions to RDMA at the initiator
  - [scalar] `falcon.cid$0.counter.pull_request_to_ulp`
    - Unit: # requests
    - Reducer: Sum
    - Number of pull requests to RDMA at the target
  - [scalar] `falcon.cid$0.counter.push_data_to_ulp`
    - Unit: # data
    - Reducer: Sum
    - Number of push data to RDMA at the target
  - [scalar] `falcon.cid$0.counter.pull_response_from_ulp`
    - Unit: # response
    - Reducer: Sum
    - Number of pull responses from RDMA at the target
  - [scalar] `falcon.cid$0.counter.acks_from_ulp`
    - Unit: # ACKs
    - Reducer: Sum
    - Number of acks from RDMA at the target
  - [scalar] `falcon.cid$0.counter.nacks_from_ulp`
    - Unit: # NACKs
    - Reducer: Sum
    - Number of nacks from RDMA at the target

- Flag: `enable_per_connection_network_counters`
  - [scalar] `falcon.cid$0.counter.rx_packets`
    - Unit: # packets
    - Reducer: Sum
    - Number of packets received by the connection
  - [scalar] `falcon.cid$0.counter.tx_packets`
    - Unit: # packets
    - Reducer: Sum
    - Number of packets transmitted by the connection
  - [vector] `falcon.cid$0.counter.rx_bytes`
    - Unit: # packets
    - Accumulative: Yes
    - Number of bytes received by the connection
  - [vector] `falcon.cid$0.counter.tx_bytes`
    - Unit: # packets
    - Accumulative: Yes
    - Number of bytes transmitted by the connection

- Flag: `enable_per_connection_ack_nack_counters`
  - [scalar] `falcon.cid$0.counter.rx_acks`
    - Unit: # ACKs
    - Reducer: Sum
    - Number of acks received by the connection
  - [scalar] `falcon.cid$0.counter.tx_acks`
    - Unit: # ACKs
    - Reducer: Sum
    - Number of acks transmitted by the connection
  - [scalar] `falcon.cid$0.counter.rx_nacks`
    - Unit: # NACKs
    - Reducer: Sum
    - Number of nacks received by the connection
  - [scalar] `falcon.cid$0.counter.tx_nacks`
    - Unit: # NACKs
    - Reducer: Sum
    - Number of nacks transmitted by the connection

- Flag: `enable_per_connection_initiator_txn_counters`
  - [scalar] `falcon.cid$0.counter.initiator_tx_push_solicited_request`
    - Unit: # requests
    - Reducer: Sum
    - Push solicited requests transmitted
  - [scalar] `falcon.cid$0.counter.initiator_tx_push_unsolicited_data`
    - Unit: # data
    - Reducer: Sum
    - Push unsolicited data transmitted
  - [scalar] `falcon.cid$0.counter.initiator_tx_pull_request`
    - Unit: # requests
    - Reducer: Sum
    - Pull requests transmitted
  - [scalar] `falcon.cid$0.counter.initiator_tx_push_solicited_data`
    - Unit: # data
    - Reducer: Sum
    - Push solicited data transmitted
  - [scalar] `falcon.cid$0.counter.initiator_rx_push_grant`
    - Unit: # grants
    - Reducer: Sum
    - Push grants received
  - [scalar] `falcon.cid$0.counter.initiator_rx_pull_data`
    - Unit: # data
    - Reducer: Sum
    - Pull data received

- Flag: `enable_per_connection_target_txn_counters`
  - [scalar] `falcon.cid$0.counter.target_tx_pull_data`
    - Unit: # data
    - Reducer: Sum
    - Pull data transmitted
  - [scalar] `falcon.cid$0.counter.target_tx_push_grant`
    - Unit: # grants
    - Reducer: Sum
    - Push grants transmitted
  - [scalar] `falcon.cid$0.counter.target_rx_push_solicited_request`
    - Unit: # requests
    - Reducer: Sum
    - Push solicited requests received
  - [scalar] `falcon.cid$0.counter.target_rx_push_solicited_data`
    - Unit: # data
    - Reducer: Sum
    - Push solicited data received
  - [scalar] `falcon.cid$0.counter.target_rx_push_unsolicited_data`
    - Unit: # data
    - Reducer: Sum
    - Push unsolicited data received
  - [scalar] `falcon.cid$0.counter.target_rx_pull_request`
    - Unit: # requests
    - Reducer: Sum
    - Pull requests received

- Flag: `enable_per_connection_rue_counters`
  - [scalar] `falcon.cid$0.counter.rue_ack_events`
    - Unit: # RUE events
    - Reducer: Sum
    - Ack events enqueued
  - [scalar] `falcon.cid$0.counter.rue_nack_events`
    - Unit: # RUE events
    - Reducer: Sum
    - Nack events enqueued
  - [scalar] `falcon.cid$0.counter.rue_retransmit_events`
    - Unit: # RUE events
    - Reducer: Sum
    - Retransmit events enqueued
  - [scalar] `falcon.cid$0.counter.rue_eack_events`
    - Unit: # RUE events
    - Reducer: Sum
    - Eack events enqueued
  - [scalar] `falcon.cid$0.counter.rue_eack_drop_events`
    - Unit: # RUE events
    - Reducer: Sum
    - Eack drop events enqueued
  - [vector] `falcon.cid$0.counter.rue_responses`
    - Unit: # RUE responses
    - Accumulative: Yes
    - Responses received from the RUE

- Flag: `enable_per_connection_rue_drop_counters`
  - [vector] `falcon.cid$0.counter.rue_enqueue_attempts`
    - Unit: # attempts
    - Accumulative: Yes
    - Event enqueue attempts
  - [vector] `falcon.cid$0.counter.event_drops_ack`
    - Unit: # drops
    - Accumulative: Yes
    - Ack events dropped
  - [vector] `falcon.cid$0.counter.event_drops_nack`
    - Unit: # drops
    - Accumulative: Yes
    - Nack events dropped
  - [vector] `falcon.cid$0.counter.event_drops_retransmits`
    - Unit: # drops
    - Accumulative: Yes
    - Retransmit events dropped
  - [vector] `falcon.cid$0.counter.event_drops_eack`
    - Unit: # drops
    - Accumulative: Yes
    - Eack events dropped
  - [vector] `falcon.cid$0.counter.event_drops_eack_drop`
    - Unit: # drops
    - Accumulative: Yes
    - Eack drop events dropped

- Flag: `enable_per_connection_ack_reason_counters`
  - [vector] `falcon.cid$0.counter.acks_generated_due_to_timeout`
    - Unit: # ACKs
    - Accumulative: Yes
    - Acks generated due to ack coalescing timeout
  - [vector] `falcon.cid$0.counter.acks_generated_due_to_ar`
    - Unit: # ACKs
    - Accumulative: Yes
    - Acks generated due to ack request bit set
  - [vector] `falcon.cid$0.counter.acks_generated_due_to_coalescing_counter`
    - Unit: # ACKs
    - Accumulative: Yes
    - Acks generated due to ack coalescing counter

- Flag: `enable_per_connection_packet_drop_counters`
  - [scalar] `falcon.cid$0.counter.total_rx_dropped_pkts`
    - Unit: # packets
    - Reducer: Sum
    - Total number of packets dropped by Falcon
  - [scalar] `falcon.cid$0.counter.rx_resource_dropped_transaction_pkts`
    - Unit: # packets
    - Reducer: Sum
    - Packets dropped due to insufficient resources
  - [scalar] `falcon.cid$0.counter.rx_window_dropped_transaction_pkts`
    - Unit: # packets
    - Reducer: Sum
    - Packets dropped due to sliding window
  - [scalar] `falcon.cid$0.counter.rx_duplicate_dropped_transaction_pkts`
    - Unit: # packets
    - Reducer: Sum
    - Packets dropped because they were duplicate
  - [scalar] `falcon.cid$0.counter.rx_duplicate_dropped_ack_pkts`
    - Unit: # packets
    - Reducer: Sum
    - Ack packets dropped because they were duplicate
  - [scalar] `falcon.cid$0.counter.rx_stale_dropped_ack_pkts_with_zero_num_acked`
    - Unit: # packets
    - Reducer: Sum
    - Ack packets dropped because they were stale with zero num_acked
  - [scalar] `falcon.cid$0.counter.rx_duplicate_dropped_nack_pkts`
    - Unit: # packets
    - Reducer: Sum
    - Nack packets dropped because they were duplicate

- Flag: `enable_per_connection_retx_counters`
  - [scalar] `falcon.cid$0.counter.tx_timeout_retransmitted`
    - Unit: # retransmission
    - Reducer: Sum
    - Number of timeout retransmissions
  - [scalar] `falcon.cid$0.counter.tx_early_ooo_dis_retransmitted`
    - Unit: # retransmission
    - Reducer: Sum
    - Number of early retransmissions due to OOO distance
  - [scalar] `falcon.cid$0.counter.tx_early_rack_retransmitted`
    - Unit: # retransmission
    - Reducer: Sum
    - Number of early retransmissions due to RACK
  - [scalar] `falcon.cid$0.counter.tx_early_nack_retransmitted`
    - Unit: # retransmission
    - Reducer: Sum
    - Number of early retransmissions due to NACK
  - [scalar] `falcon.cid$0.counter.tx_early_tlp_retransmitted`
    - Unit: # retransmission
    - Reducer: Sum
    - Number of early retransmissions due to TLP
  - [scalar] `falcon.cid$0.counter.tx_ulp_retransmitted`
    - Unit: # retransmission
    - Reducer: Sum
    - Number of retransmissions due to ULP timeout

- Flag: `enable_per_connection_solicitation_counters`
  - [vector] `falcon.solicitation_window_occupied_bytes`
    - Unit: Bytes
    - Accumulative: No
    - Number of bytes occupied in the solicitation window
  - [vector] `falcon.solicitation_request_window_occupied_bytes`
    - Unit: Bytes
    - Accumulative: No
    - Number of bytes occupied in the solicitation request window
  - [vector] `falcon.cid$0.counter.solicitation_window_occupied_bytes`
    - Unit: Bytes
    - Accumulative: No
    - Number of bytes occupied by the connection in the solicitation window
  - [vector] `falcon.cid$0.solicitation_received_bytes_estimator`
    - Unit: Bytes
    - Accumulative: No
    - Number of bytes estimated received by the given connection
  - [vector] `falcon.cid$0.solicitation_bytes_estimator`
    - Unit: Bytes
    - Accumulative: No
    - Number of bytes estimated solicited by the given connection
  - [vector] `falcon.cid$0.solicitation_alpha`
    - Unit: Unitless
    - Accumulative: No
    - Current value of connection alpha for carving solicitation window
  - [vector] `falcon.cid$0.solicitation_limit_bytes`
    - Unit: Bytes
    - Accumulative: No
    - Computed connection limit of solicitation bytes

- Flag: `enable_per_connection_resource_credit_counters`
  - [vector] `falcon.cid$0.counter.tx_packet_credits.ulp_requests`
    - Unit: # credits
    - Accumulative: No
    - The number of remaining Tx packet credits for ULP request resource.
  - [vector] `falcon.cid$0.counter.tx_packet_credits.ulp_data`
    - Unit: # credits
    - Accumulative: No
    - The number of remaining Tx packet credits for ULP data resource.
  - [vector] `falcon.cid$0.counter.tx_packet_credits.network_requests`
    - Unit: # credits
    - Accumulative: No
    - The number of remaining Tx packet credits for network request resource.
  - [vector] `falcon.cid$0.counter.tx_buffer_credits.ulp_requests`
    - Unit: # credits
    - Accumulative: No
    - The number of remaining Tx buffer credits for ULP request resource.
  - [vector] `falcon.cid$0.counter.tx_buffer_credits.ulp_data`
    - Unit: # credits
    - Accumulative: No
    - The number of remaining Tx buffer credits for ULP data resource.
  - [vector] `falcon.cid$0.counter.tx_buffer_credits.network_requests`
    - Unit: # credits
    - Accumulative: No
    - The number of remaining Tx buffer credits for network request resource.
  - [vector] `falcon.cid$0.counter.rx_packet_credits.ulp_requests`
    - Unit: # credits
    - Accumulative: No
    - The number of remaining Rx packet credits for ULP request resource.
  - [vector] `falcon.cid$0.counter.rx_packet_credits.network_requests`
    - Unit: # credits
    - Accumulative: No
    - The number of remaining Rx packet credits for network request resource.
  - [vector] `falcon.cid$0.counter.rx_buffer_credits.ulp_requests`
    - Unit: # credits
    - Accumulative: No
    - The number of remaining Rx buffer credits for ULP request resource.
  - [vector] `falcon.cid$0.counter.rx_buffer_credits.network_requests`
    - Unit: # credits
    - Accumulative: No
    - The number of remaining Rx buffer credits for network request resource.

Flag: `enable_per_connection_cwnd_pause`
  - [scalar] `falcon.cid$0.counter.connection_cwnd_pause_time`
    - Unit: Nanoseconds
    - Reducer: Sum
    - Duration that connection was paused due to cwnd

Flag: `enable_per_connection_max_rsn_difference`
  - [scalar] `falcon.cid$0.counter.max_pull_push_rsn_distance`
    - Unit: Distance (in request sequence number)
    - Reducer: Max
    - The max distance between push and pull RSN.

Flag: `enable_per_connection_scheduler_queue_length`
  - [vector] `falcon.cid$0.counter.pull_and_ordered_push_request_queue`
    - Unit: # packets
    - Accumulative: No
    - Pull and ordered push request queue length in connection scheduler per connection
  - [vector] `falcon.cid$0.counter.unordered_push_request_queue`
    - Unit: # packets
    - Accumulative: No
    - Unordered push request queue length in connection scheduler per connection
  - [vector] `falcon.cid$0.counter.push_data_queue`
    - Unit: # packets
    - Accumulative: No
    - Push data queue length in connection scheduler per connection
  - [vector] `falcon.cid$0.counter.push_grant_queue`
    - Unit: # packets
    - Accumulative: No
    - Push grant queue length in connection scheduler per connection
  - [vector] `falcon.cid$0.counter.pull_data_queue`
    - Unit: # packets
    - Accumulative: No
    - Pull data queue length in connection scheduler per connection

Flag: `enable_per_connection_backpressure_alpha_carving_limits`
  - [vector] `falcon.cid$0.alpha_carving_request_tx_packet_limit`
    - Unit: # credits
    - Accumulative: No
    - Request TX packet resource limit computed by the per-connection backpressure alpha carving
  - [vector] `falcon.cid$0.alpha_carving_request_tx_buffer_limit`
    - Unit: # credits
    - Accumulative: No
    - Request TX buffer resource limit computed by the per-connection backpressure alpha carving
  - [vector] `falcon.cid$0.alpha_carving_request_rx_packet_limit`
    - Unit: # credits
    - Accumulative: No
    - Request RX packet resource limit computed by the per-connection backpressure alpha carving
  - [vector] `falcon.cid$0.alpha_carving_request_rx_buffer_limit`
    - Unit: # credits
    - Accumulative: No
    - Request RX buffer resource limit computed by the per-connection backpressure alpha carving
  - [vector] `falcon.cid$0.alpha_carving_response_tx_packet_limit`
    - Unit: # credits
    - Accumulative: No
    - Response TX packet resource limit computed by the per-connection backpressure alpha carving
  - [vector] `falcon.cid$0.alpha_carving_response_tx_buffer_limit`
    - Unit: # credits
    - Accumulative: No
    - Response TX buffer resource limit computed by the per-connection backpressure alpha carving

Flag: `enable_per_connection_window_usage`
  - [vector] `falcon.cid$0.counter.request_window_usage`
    - Unit: # packets (distance in packet sequence number)
    - Accumulative: No
    - Per-connection usage of the request sliding window
  - [vector] `falcon.cid$0.counter.data_window_usage`
    - Unit: # packets (distance in packet sequence number)
    - Accumulative: No
    - Per-connection usage of the data sliding window

Flag: `enable_per_connection_initial_tx_rsn_timeline`
  - [vector] `falcon.cid$0.counter.pull_request_inital_tx_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of initial transmission of Pull requests
  - [vector] `falcon.cid$0.counter.push_unsolicited_data_inital_tx_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of initial transmission of Push unsolicited data

Flag: `enable_per_connection_rx_from_ulp_rsn_timeline`
  - [vector] `falcon.cid$0.counter.pull_request_rx_from_ulp_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of pull requests when they arrive to Falcon from ULP
  - [vector] `falcon.cid$0.counter.push_unsolicited_data_rx_from_ulp_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of push unsolicited data when they arrive to Falcon from ULP

Flag: `enable_per_connection_retx_rsn_timeline`
  - [vector] `falcon.cid$0.counter.pull_request_retx_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of pull request retransmission
  - [vector] `falcon.cid$0.counter.pull_request_ooo_retx_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of pull request retransmission due to OOO
  - [vector] `falcon.cid$0.counter.pull_request_rack_retx_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of pull request retransmission due to RACK
  - [vector] `falcon.cid$0.counter.pull_request_tlp_retx_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of pull request retransmission due to TLP
  - [vector] `falcon.cid$0.counter.pull_request_nack_retx_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of pull request retransmission due to NACK
  - [vector] `falcon.cid$0.counter.pull_request_rto_retx_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of pull request retransmission due to RTO
  - [vector] `falcon.cid$0.counter.pull_request_ulp_rto_retx_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of pull request retransmission due to ULP RTO
  - [vector] `falcon.cid$0.counter.push_unsolicited_data_retx_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of push unsolicited data retransmission
  - [vector] `falcon.cid$0.counter.push_unsolicited_data_ooo_retx_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of push unsolicited data retransmission due to OOO
  - [vector] `falcon.cid$0.counter.push_unsolicited_data_rack_retx_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of push unsolicited data retransmission due to RACK
  - [vector] `falcon.cid$0.counter.push_unsolicited_data_tlp_retx_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of push unsolicited data retransmission due to TLP
  - [vector] `falcon.cid$0.counter.push_unsolicited_data_nack_retx_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of push unsolicited data retransmission due to NACK
  - [vector] `falcon.cid$0.counter.push_unsolicited_data_rto_retx_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of push unsolicited data retransmission due to RTO
  - [vector] `falcon.cid$0.counter.push_unsolicited_data_ulp_rto_retx_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of push unsolicited data retransmission due to ULP RTO

Flag: `enable_per_connection_rsn_receive_timeline`
  - [vector] `falcon.cid$0.counter.pull_request_recv_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of received pull request
  - [vector] `falcon.cid$0.counter.push_data_rsn_recv_rsn`
    - Unit: Sequence number
    - Accumulative: No
    - RSN of received push unsolicited data

Flag: `enable_connection_scheduler_max_delayed_packet_stats`
  - [vector] `falcon.max_queueing_delay_by_type.$0`
    - Unit: Microseconds
    - Accumulative: Yes
    - Maximum queuing delay time for each packet type
  - [vector] `falcon.serialized_packet_count_by_type_for_max_queued_packet.$0.$1`
    - Unit: # packets
    - Accumulative: No
    - Scheduled packet count by type ($1) during the queuing of the above max delayed packet in each type ($0)
  - [vector] `falcon.enqueue_time_queue_length_for_max_queued_packet.$0.$1`
    - Unit: # packets
    - Accumulative: No
    - Queue length per scheduler queue type ($1) when the queuing of the above max delayed packet in each type ($0) is enqueued

Flag: `enable_resource_manager_ema_occupancy`
  - [vector] `falcon.resources.rx_buffer_ema`
    - Unit: # credits
    - Accumulative: No
    - EMA of RX buffer for ncwnd calculation
  - [vector] `falcon.resources.rx_packet_ema`
    - Unit: # credits
    - Accumulative: No
    - EMA of RX packet for ncwnd calculation
  - [vector] `falcon.resources.tx_packet_ema`
    - Unit: # credits
    - Accumulative: No
    - EMA of TX packet for ncwnd calculation
  - [vector] `falcon.resources.network_region_quantized_occupancy`
    - Unit: # credits
    - Accumulative: No
    - Quantized buffer occupancy for network ncwnd calculation

Flag: `enable_global_resource_credits_timeline`
  - [vector] `falcon.resources.tx_packet_credits.ulp_requests`
    - Unit: # credits
    - Accumulative: No
    - Total global TX packet credits for ULP requests in Falcon
  - [vector] `falcon.resources.tx_packet_credits.ulp_data`
    - Unit: # credits
    - Accumulative: No
    - Total global TX packet credits for ULP data in Falcon
  - [vector] `falcon.resources.tx_packet_credits.network_requests`
    - Unit: # credits
    - Accumulative: No
    - Total global TX packet credits for network requests in Falcon
  - [vector] `falcon.resources.tx_buffer_credits.ulp_requests`
    - Unit: # credits
    - Accumulative: No
    - Total global TX buffer credits for ULP requests in Falcon
  - [vector] `falcon.resources.tx_buffer_credits.ulp_data`
    - Unit: # credits
    - Accumulative: No
    - Total global TX buffer credits for ULP data in Falcon
  - [vector] `falcon.resources.tx_buffer_credits.network_requests`
    - Unit: # credits
    - Accumulative: No
    - Total global TX buffer credits for network requests in Falcon
  - [vector] `falcon.resources.rx_packet_credits.ulp_requests`
    - Unit: # credits
    - Accumulative: No
    - Total global RX packet credits for ULP requests in Falcon
  - [vector] `falcon.resources.rx_packet_credits.network_requests`
    - Unit: # credits
    - Accumulative: No
    - Total global RX packet credits for network requests in Falcon
  - [vector] `falcon.resources.rx_buffer_credits.ulp_requests`
    - Unit: # credits
    - Accumulative: No
    - Total global RX buffer credits for ULP requests in Falcon
  - [vector] `falcon.resources.rx_buffer_credits.network_requests`
    - Unit: # credits
    - Accumulative: No
    - Total global RX buffer credits for network requests in Falcon

- Flag: `enable_inter_host_rx_scheduler_queue_length`
  - [vector] `falcon.inter_host_rx_scheduler.bifurcation$0.outstanding_packets`
    - Unit: # packets
    - Accumulative: No
    - Queue length of the inter host RX scheduler in Falcon

### Traffic Shaper

N/A

### Packet Builder

- Flag: `enable_scalar_packet_delay`
  - [scalar] `packet_builder.scalar_rx_packet_delay`
    - Unit: Seconds
    - Reducer: Mean
    - Average packet delay on the RX path
  - [scalar] `packet_builder.scalar_tx_packet_delay`
    - Unit: Seconds
    - Reducer: Mean
    - Average packet delay on the TX path

- Flag: `enable_vector_packet_delay`
  - [vector] `packet_builder.rx_packet_delay`
    - Units: Seconds
    - Accumulative: No
    - Packet delays on RX path
  - [vector] `packet_builder.tx_packet_delay`
    - Units: Seconds
    - Accumulative: No
    - Packet delays on TX path

- Flag: `enable_queue_length`
  - [vector] `packet_builder.priority$0.tx_queue_length`
    - Unit: # packets
    - Accumulative: No
    - Packet queue lengths on the TX path

- Flag: `enable_discard_and_drops`
  - [scalar] `packet_builder.wrong_destination_ip_address_discards`
    - Unit: # packets
    - Reducer: Sum
    - Total discards due to wrong destination ip address
  - [scalar] `packet_builder.wrong_outgoing_packet_size_discards`
    - Unit: # packets
    - Reducer: Sum
    - Total discards due to wrong outgoing packet size
  - [scalar] `packet_builder.random_drops`
    - Unit: # packets
    - Reducer: Sum
    - Total drops due to random sampling
  - [scalar] `packet_builder.total_discards`
    - Unit: # packets
    - Reducer: Sum
    - Total drops/discards due to any reason

- Flag: `enable_scalar_tx_rx_packets_bytes`
  - [scalar] `packet_builder.total_tx_packets`
    - Unit: # packets
    - Reducer: Sum
    - Total Tx Packets
  - [scalar] `packet_builder.total_rx_packets`
    - Unit: # packets
    - Reducer: Sum
    - Total Rx Packets
  - [scalar] `packet_builder.total_tx_bytes`
    - Unit: Bytes
    - Reducer: Sum
    - Total Tx Bytes
  - [scalar] `packet_builder.total_rx_bytes`
    - Unit: Bytes
    - Reducer: Sum
    - Total Rx Bytes

- Flag: `enable_vector_tx_rx_bytes`
  - [vector] `packet_builder.tx_bytes`
    - Unit: Bytes
    - Accumulative: Yes
    - Tx Bytes
  - [vector] `packet_builder.rx_bytes`
    - Unit: Bytes
    - Accumulative: Yes
    - Rx Bytes

- Flag: `enable_pfc`
  - [vector] `packet_builder.priority$0.pfc_pause_start`
    - Units: # PFC
    - Accumulative: Yes
    - The number of PFC paused
  - [vector] `packet_builder.priority$0.pfc_pause_end`
    - Units: # PFC
    - Accumulative: Yes
    - The number of PFC resumed

- Flag: `enable_xoff_duration`
  - [scalar] `packet_builder.total_xoff_duration`
    - Units: Microseconds
    - Reducer: Sum
    - Total xoff to Falcon duration

## Network Router

We collect per-port stats and per-port per-queue stats in Network router.
In addition, we record the packet drop stats in routing pipeline.

### Port

- Flag: `enable_scalar_per_port_tx_rx_packets`
  - [scalar] `router.port$0.tx_packets`
    - Unit: # packets
    - Reducer: Sum
    - Number of packets transmitted by the port
  - [scalar] `router.port$0.rx_packets`
    - Unit: # packets
    - Reducer: Sum
    - Number of packets received by the port

- Flag: `enable_vector_per_port_tx_rx_bytes`
  - [vector] `router.port$0.tx_bytes`
    - Unit: Bytes
    - Accumulative: Yes
    - Cumulative number of bytes transmitted by the port
  - [vector] `router.port$0.rx_bytes`
    - Unit: Bytes
    - Accumulative: Yes
    - Cumulative number of bytes received by the port

- Flag: `enable_port_stats_collection`
  - The metrics are collected every `port_stats_collection_interval_us`.
  - [vector] `router.port$0.queue_bytes`
    - Unit: Bytes
    - Accumulative: No
    - Queued bytes per-port at periodic intervals
  - [vector] `router.port$0.capacity`
    - Unit: bps
    - Accumulative: No
    - Port capacity at periodic intervals

- Flag: `enable_per_port_ingress_discards`
  - [scalar] `router.port$0.ingress_packet_discards`
    - Unit: # packets
    - Reducer: Sum
    - Number of ingress discards in the port

### Tx Queue

- Flag: `enable_per_port_per_queue_stats`
  - [vector] `router.port$0.queue$1.enqueue_fails`
    - Unit: # events
    - Accumulative: Yes
    - Accumulative out packet discards per port per queue. Currently, we only
    have one Tx queue per port with index 0. The ingress packet may be dropped
    because MMU does not have enough space to enqueue it
  - [vector] `router.port$0.queue$1.tx_queue_length`
    - Unit: # packets
    - Accumulative: No
    - Tx queue length
  - [vector] `router.port$0.queue$1.tx_queue_bytes`
    - Unit: Bytes
    - Accumulative: No
    - TX queue bytes
  - [vector] `router.port$0.queue$1.packet_queueing_delay`
    - Unit: Seconds
    - Accumulative: No
    - Tx queueing delay
  - [vector] `router.port$0.queue$1.tx_bytes`
    - Unit: Bytes
    - Accumulative: Yes
    - Cumulative Tx bytes
  - [vector] `router.port$0.queue$1.queue_occupancy`
    - Unit: Cells
    - Accumulative: No
    - Queue occupancy
  - [vector] `outer.port$0.queue$1.dynamic_limit`
    - Unit: Cells
    - Accumulative: No
    - Dynamic limit used for alpha carving computed as `free_cells * alpha`
    which is compared to queue occupancy to determine whether to drop a packet

- Flag: `enable_pfc_stats`
  - [vector] `router.mmu.port$0.queue$1.triggered_pfc`
    - Unit: # PFCs
    - Accumulative: Yes
    - The number of PFCs paused on the queue
  - [vector] `router.mmu.port$0.queue$1.resumed_pfc`
    - Unit: # PFCs
    - Accumulative: Yes
    - The number of PFCs resumed on the queue

### Routing Pipeline

- Flag: `enable_packet_discards`
  - [scalar] `router.routing_pipeline.no_route_discards`
    - Unit: # discards
    - Reducer: Sum
    - The total number of dropped packets due to no route found
  - [scalar] `router.routing_pipeline.ttl_limit_discards`
    - Unit: # discards
    - Reducer: Sum
    - The total number of dropped packets due to exceeding TTL limit
  - [scalar] `router.routing_pipeline.total_discards`
    - Unit: # discards
    - Reducer: Sum
    - The total number of packets dropped by routing pipeline
