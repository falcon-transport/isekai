# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//isekai:__subpackages__"])

licenses(["notice"])

cc_library(
    name = "stats_manager",
    srcs = ["stats_manager.cc"],
    hdrs = [
        "stats_manager.h",
    ],
    deps = [
        ":connection_scheduler_types",
        ":falcon_types",
        ":falcon_utils",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:status_util",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_bitmap",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_counters",
        "//isekai/host/falcon:falcon_histograms",
        "//isekai/host/falcon:falcon_resource_credits",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
    ],
)

cc_library(
    name = "falcon_model",
    srcs = ["falcon_model.cc"],
    hdrs = [
        "falcon_model.h",
    ],
    deps = [
        ":falcon_types",
        ":falcon_utils",
        ":inter_host_rx_scheduler",
        "//isekai/common:config_cc_proto",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:status_util",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_factories",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_histograms",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_glog//:glog",
    ],
)

cc_library(
    name = "resource_manager",
    srcs = ["resource_manager.cc"],
    hdrs = ["resource_manager.h"],
    deps = [
        ":falcon_utils",
        "//isekai/common:config_cc_proto",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:status_util",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_resource_credits",
        "//isekai/host/falcon/rue",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_glog//:glog",
    ],
)

cc_library(
    name = "ack_coalescing_engine",
    srcs = ["ack_coalescing_engine.cc"],
    hdrs = ["ack_coalescing_engine.h"],
    deps = [
        ":ack_nack_scheduler",
        ":falcon_types",
        ":falcon_utils",
        "//isekai/common:config_cc_proto",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:status_util",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_bitmap",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon/gen2:falcon_types",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
    ],
)

cc_test(
    name = "ack_coalescing_engine_test",
    srcs = ["ack_coalescing_engine_test.cc"],
    deps = [
        ":ack_coalescing_engine",
        ":falcon_model",
        ":falcon_types",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_testing_helpers",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "rate_update_engine",
    srcs = ["rate_update_engine.cc"],
    hdrs = ["rate_update_engine.h"],
    deps = [
        ":falcon_types",
        ":rate_update_engine_adapter",
        "//isekai/common:config_cc_proto",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:status_util",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:rue_algorithm_factories",
        "//isekai/host/falcon/rue",
        "//isekai/host/falcon/rue:latency_generator",
        "//isekai/host/falcon/rue/algorithm",
        "//isekai/host/falcon/rue/algorithm:config_cc_proto",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_glog//:glog",
    ],
)

cc_library(
    name = "ack_nack_scheduler",
    srcs = ["ack_nack_scheduler.cc"],
    hdrs = [
        "ack_nack_scheduler.h",
    ],
    deps = [
        ":connection_scheduler_types",
        "//isekai/common:packet",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_histograms",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_glog//:glog",
    ],
)

cc_library(
    name = "falcon_types",
    hdrs = ["falcon_types.h"],
    deps = [
        ":connection_scheduler_types",
        "//isekai/common:constants",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_bitmap",
        "//isekai/host/falcon:gating_variable",
        "//isekai/host/falcon/rue",
        "@com_google_absl//absl/container:btree",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/time",
    ],
)

cc_library(
    name = "connection_state_manager",
    srcs = ["connection_state_manager.cc"],
    hdrs = ["connection_state_manager.h"],
    deps = [
        ":falcon_types",
        "//isekai/common:config_cc_proto",
        "//isekai/common:constants",
        "//isekai/common:model_interfaces",
        "//isekai/common:status_util",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/functional:bind_front",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
)

cc_library(
    name = "connection_scheduler_types",
    srcs = ["connection_scheduler_types.cc"],
    hdrs = ["connection_scheduler_types.h"],
    deps = [
        "//isekai/host/falcon",
        "@com_google_absl//absl/container:btree",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_glog//:glog",
    ],
)

cc_library(
    name = "weighted_round_robin_policy",
    hdrs = ["weighted_round_robin_policy.h"],
    deps = [
        ":connection_scheduler_types",
        "//isekai/host/falcon:falcon_component_interfaces",
        "@com_google_absl//absl/container:btree",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_glog//:glog",
    ],
)

cc_library(
    name = "inter_host_rx_scheduler",
    srcs = ["inter_host_rx_scheduler.cc"],
    hdrs = [
        "inter_host_rx_scheduler.h",
    ],
    deps = [
        ":weighted_round_robin_policy",
        "//isekai/common:config_cc_proto",
        "//isekai/common:model_interfaces",
        "//isekai/host/falcon:falcon_component_interfaces",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/functional:any_invocable",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_glog//:glog",
    ],
)

cc_library(
    name = "admission_control_manager",
    srcs = ["admission_control_manager.cc"],
    hdrs = ["admission_control_manager.h"],
    deps = [
        ":falcon_utils",
        "//isekai/common:config_cc_proto",
        "//isekai/common:model_interfaces",
        "//isekai/common:status_util",
        "//isekai/common:token_bucket",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_histograms",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
    ],
)

cc_library(
    name = "packet_reliability_manager",
    srcs = ["packet_reliability_manager.cc"],
    hdrs = ["packet_reliability_manager.h"],
    deps = [
        ":connection_scheduler_types",
        ":falcon_types",
        ":falcon_utils",
        "//isekai/common:config_cc_proto",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:net_address",
        "//isekai/common:packet",
        "//isekai/common:status_util",
        "//isekai/common:token_bucket",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_bitmap",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon/rue",
        "@com_google_absl//absl/container:btree",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_prod",
    ],
)

cc_library(
    name = "buffer_reorder_engine",
    srcs = ["buffer_reorder_engine.cc"],
    hdrs = ["buffer_reorder_engine.h"],
    deps = [
        ":falcon_utils",
        "//isekai/common:model_interfaces",
        "//isekai/common:status_util",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "@com_google_absl//absl/container:btree",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/status",
        "@com_google_glog//:glog",
    ],
)

cc_library(
    name = "round_robin_arbiter",
    srcs = ["round_robin_arbiter.cc"],
    hdrs = ["round_robin_arbiter.h"],
    deps = [
        "//isekai/common:config_cc_proto",
        "//isekai/common:environment",
        "//isekai/common:status_util",
        "//isekai/host/falcon:falcon_component_interfaces",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/time",
    ],
)

cc_library(
    name = "falcon_utils",
    srcs = ["falcon_utils.cc"],
    hdrs = ["falcon_utils.h"],
    deps = [
        ":connection_scheduler_types",
        ":falcon_types",
        "//isekai/common:packet",
        "//isekai/common:status_util",
        "//isekai/host/falcon",
    ],
)

cc_library(
    name = "connection_scheduler",
    srcs = ["connection_scheduler.cc"],
    hdrs = ["connection_scheduler.h"],
    deps = [
        ":connection_scheduler_types",
        ":falcon_types",
        ":falcon_utils",
        ":weighted_round_robin_policy",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:status_util",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_histograms",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/time",
        "@com_google_glog//:glog",
    ],
)

cc_test(
    name = "connection_scheduler_test",
    srcs = ["connection_scheduler_test.cc"],
    deps = [
        ":falcon_types",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon:connection_state_utils",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_testing_helpers",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "retransmission_scheduler",
    srcs = ["retransmission_scheduler.cc"],
    hdrs = ["retransmission_scheduler.h"],
    deps = [
        ":connection_scheduler_types",
        ":falcon_types",
        ":falcon_utils",
        ":weighted_round_robin_policy",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:status_util",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_histograms",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/time",
        "@com_google_glog//:glog",
    ],
)

cc_test(
    name = "retransmission_scheduler_test",
    srcs = ["retransmission_scheduler_test.cc"],
    deps = [
        ":falcon_model",
        ":falcon_types",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_testing_helpers",
        "//isekai/host/rnic:connection_manager",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "rate_update_engine_adapter",
    srcs = ["rate_update_engine_adapter.cc"],
    hdrs = ["rate_update_engine_adapter.h"],
    deps = [
        ":falcon_types",
        "//isekai/common:config_cc_proto",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/host/falcon:event_response_format_adapter",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon/gen2:falcon_types",
        "//isekai/host/falcon/rue",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
    ],
)

cc_library(
    name = "packet_metadata_transformer",
    srcs = ["packet_metadata_transformer.cc"],
    hdrs = ["packet_metadata_transformer.h"],
    deps = [
        "//isekai/common:config_cc_proto",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:status_util",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "@com_google_absl//absl/log:check",
    ],
)

cc_test(
    name = "packet_metadata_transformer_test",
    srcs = ["packet_metadata_transformer_test.cc"],
    deps = [
        ":falcon_model",
        ":falcon_types",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_testing_helpers",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "rate_update_engine_adapter_test",
    srcs = ["rate_update_engine_adapter_test.cc"],
    deps = [
        ":falcon_model",
        ":falcon_types",
        ":rate_update_engine",
        ":rate_update_engine_adapter",
        "//isekai/common:common_util",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_testing_helpers",
        "//isekai/host/falcon/rue",
        "//isekai/host/rnic:connection_manager",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "buffer_reorder_engine_test",
    srcs = ["buffer_reorder_engine_test.cc"],
    deps = [
        ":buffer_reorder_engine",
        ":falcon_model",
        ":falcon_utils",
        "//isekai/common:common_util",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_counters",
        "//isekai/host/falcon:falcon_testing_helpers",
        "//isekai/host/rnic:connection_manager",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "connection_state_manager_test",
    srcs = ["connection_state_manager_test.cc"],
    deps = [
        ":connection_state_manager",
        ":falcon_utils",
        "//isekai/common:common_util",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_testing_helpers",
        "//isekai/host/rnic:connection_manager",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "resource_manager_test",
    srcs = ["resource_manager_test.cc"],
    deps = [
        ":falcon_model",
        ":falcon_types",
        ":falcon_utils",
        ":resource_manager",
        "//isekai/common:common_util",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/fabric:constants",
        "//isekai/host/falcon",
        "//isekai/host/falcon:connection_state_utils",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_resource_credits",
        "//isekai/host/falcon:falcon_testing_helpers",
        "//isekai/host/falcon/gen2:resource_manager",
        "//isekai/host/falcon/rue",
        "//isekai/host/rdma:rdma_falcon_model",
        "//isekai/host/rnic:connection_manager",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest_main",
        "@com_google_protobuf//:protobuf",
    ],
)

cc_test(
    name = "packet_reliability_manager_test",
    srcs = ["packet_reliability_manager_test.cc"],
    deps = [
        ":connection_scheduler_types",
        ":falcon_model",
        ":falcon_types",
        ":packet_reliability_manager",
        "//isekai/common:config_cc_proto",
        "//isekai/common:constants",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon:connection_state_utils",
        "//isekai/host/falcon:falcon_bitmap",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_testing_helpers",
        "//isekai/host/falcon/rue",
        "@com_google_absl//absl/container:btree",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "rate_update_engine_test",
    srcs = ["rate_update_engine_test.cc"],
    deps = [
        ":falcon_model",
        ":falcon_utils",
        ":rate_update_engine",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:packet",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_counters",
        "//isekai/host/falcon:falcon_testing_helpers",
        "//isekai/host/falcon/rue",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "connection_scheduler_types_test",
    srcs = ["connection_scheduler_types_test.cc"],
    deps = [
        ":connection_scheduler_types",
        "@com_google_absl//absl/container:btree",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "ack_nack_scheduler_test",
    srcs = ["ack_nack_scheduler_test.cc"],
    deps = [
        ":ack_nack_scheduler",
        ":falcon_model",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_testing_helpers",
        "//isekai/host/rnic:connection_manager",
        "@com_google_absl//absl/log:check",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "admission_control_manager_test",
    srcs = ["admission_control_manager_test.cc"],
    deps = [
        ":falcon_types",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon:connection_state_utils",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_testing_helpers",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "round_robin_arbiter_test",
    srcs = ["round_robin_arbiter_test.cc"],
    deps = [
        ":falcon_model",
        ":round_robin_arbiter",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_testing_helpers",
        "//isekai/host/rnic:connection_manager",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "inter_host_rx_scheduler_test",
    srcs = ["inter_host_rx_scheduler_test.cc"],
    deps = [
        ":falcon_model",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:simple_environment",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "gen1_ack_coalescing_engine_test",
    srcs = ["gen1_ack_coalescing_engine_test.cc"],
    deps = [
        ":falcon_model",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_testing_helpers",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "gen1_reliability_manager_test",
    srcs = ["gen1_reliability_manager_test.cc"],
    deps = [
        ":falcon_model",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:packet",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_testing_helpers",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "gen1_rate_update_engine_test",
    srcs = ["gen1_rate_update_engine_test.cc"],
    deps = [
        ":falcon_model",
        ":falcon_types",
        ":rate_update_engine",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:simple_environment",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_testing_helpers",
        "//isekai/host/falcon/rue",
        "//isekai/host/rnic:connection_manager",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)
