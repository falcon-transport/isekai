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

load("@com_google_protobuf//bazel:cc_proto_library.bzl", "cc_proto_library")
load("@com_google_protobuf//bazel:proto_library.bzl", "proto_library")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//isekai:__subpackages__"])

licenses(["notice"])

cc_library(
    name = "gating_variable",
    hdrs = ["gating_variable.h"],
    deps = ["@com_google_glog//:glog"],
)

cc_test(
    name = "gating_variable_test",
    srcs = ["gating_variable_test.cc"],
    deps = [
        ":gating_variable",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "falcon_counters",
    hdrs = [
        "falcon_counters.h",
    ],
)

cc_test(
    name = "falcon_counters_test",
    srcs = ["falcon_counters_test.cc"],
    deps = [
        "//isekai/common:default_config_generator",
        "//isekai/host/falcon/gen1:falcon_model",
        "//isekai/host/falcon/gen1:falcon_utils",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "falcon_histograms",
    srcs = ["falcon_histograms.cc"],
    hdrs = [
        "falcon_histograms.h",
    ],
    deps = [
        ":falcon_counters",
        ":falcon_histograms_cc_proto",
        "//isekai/common:tdigest",
        "//isekai/host/falcon",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/strings",
    ],
)

cc_test(
    name = "falcon_histograms_test",
    srcs = ["falcon_histograms_test.cc"],
    deps = [
        ":falcon_histograms",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "falcon_resource_credits",
    srcs = [
        "falcon_resource_credits.cc",
    ],
    hdrs = [
        "falcon_resource_credits.h",
    ],
    deps = ["//isekai/common:config_cc_proto"],
)

cc_test(
    name = "falcon_resource_credits_test",
    srcs = ["falcon_resource_credits_test.cc"],
    deps = [
        ":falcon_resource_credits",
        "//isekai/common:config_cc_proto",
        "@com_google_googletest//:gtest_main",
        "@com_google_protobuf//:protobuf",
    ],
)

cc_library(
    name = "falcon_component_interfaces",
    hdrs = [
        "falcon_component_interfaces.h",
    ],
    deps = [
        ":falcon_bitmap",
        ":falcon_connection_state",
        ":falcon_counters",
        ":falcon_histograms",
        ":falcon_resource_credits",
        "//isekai/common:config_cc_proto",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/host/falcon",
        "//isekai/host/falcon/gen1:connection_scheduler_types",
        "//isekai/host/falcon/gen1:falcon_types",
        "@com_google_absl//absl/functional:any_invocable",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
    ],
)

cc_library(
    name = "falcon_connection_state",
    srcs = ["falcon_connection_state.cc"],
    hdrs = ["falcon_connection_state.h"],
    deps = [
        ":connection_state_utils",
        ":falcon_bitmap",
        ":falcon_histograms",
        ":gating_variable",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/host/falcon",
        "//isekai/host/falcon/gen1:falcon_types",
        "//isekai/host/falcon/gen1:falcon_utils",
        "//isekai/host/falcon/rue",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/time",
    ],
)

proto_library(
    name = "falcon_histograms_proto",
    srcs = ["falcon_histograms.proto"],
    deps = ["//isekai/common:tdigest_proto"],
)

cc_proto_library(
    name = "falcon_histograms_cc_proto",
    deps = [":falcon_histograms_proto"],
)

cc_library(
    name = "falcon_component_factories",
    hdrs = ["falcon_component_factories.h"],
    deps = [
        ":falcon_component_interfaces",
        "//isekai/host/falcon/gen1:ack_coalescing_engine",
        "//isekai/host/falcon/gen1:ack_nack_scheduler",
        "//isekai/host/falcon/gen1:admission_control_manager",
        "//isekai/host/falcon/gen1:buffer_reorder_engine",
        "//isekai/host/falcon/gen1:connection_scheduler",
        "//isekai/host/falcon/gen1:connection_state_manager",
        "//isekai/host/falcon/gen1:inter_host_rx_scheduler",
        "//isekai/host/falcon/gen1:packet_metadata_transformer",
        "//isekai/host/falcon/gen1:packet_reliability_manager",
        "//isekai/host/falcon/gen1:rate_update_engine",
        "//isekai/host/falcon/gen1:resource_manager",
        "//isekai/host/falcon/gen1:retransmission_scheduler",
        "//isekai/host/falcon/gen1:round_robin_arbiter",
        "//isekai/host/falcon/gen1:stats_manager",
        "//isekai/host/falcon/gen2:ack_coalescing_engine",
        "//isekai/host/falcon/gen2:connection_state_manager",
        "//isekai/host/falcon/gen2:falcon_component_interfaces",
        "//isekai/host/falcon/gen2:packet_metadata_transformer",
        "//isekai/host/falcon/gen2:rate_update_engine",
        "//isekai/host/falcon/gen2:reliability_manager",
        "//isekai/host/falcon/gen2:resource_manager",
        "//isekai/host/falcon/gen2:ulp_backpressure_manager",
        "@com_google_absl//absl/log",
    ],
)

cc_library(
    name = "falcon_bitmap",
    srcs = ["falcon_bitmap.cc"],
    hdrs = ["falcon_bitmap.h"],
    deps = [
        "//isekai/common:constants",
        "@com_google_absl//absl/log:check",
        "@com_google_glog//:glog",
    ],
)

cc_library(
    name = "falcon_component_test_infrastructure",
    testonly = 1,
    srcs = ["falcon_component_test_infrastructure.cc"],
    hdrs = ["falcon_component_test_infrastructure.h"],
    deps = [
        ":falcon_resource_credits",
        "//isekai/common:common_util",
        "//isekai/common:config_cc_proto",
        "//isekai/common:constants",
        "//isekai/common:default_config_generator",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:net_address",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/fabric:constants",
        "//isekai/host/falcon",
        "//isekai/host/falcon/gen1:falcon_model",
        "//isekai/host/falcon/gen1:falcon_types",
        "//isekai/host/falcon/gen1:resource_manager",
        "//isekai/host/falcon/gen2:falcon_model",
        "//isekai/host/rdma:rdma_component_interfaces",
        "//isekai/host/rdma:rdma_falcon_model",
        "//isekai/host/rnic:connection_manager",
        "//isekai/host/rnic:traffic_shaper_model",
        "@com_google_absl//absl/algorithm:container",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_glog//:glog",
        "@com_google_googletest//:gtest",
        "@omnetpp",
    ],
)

cc_test(
    name = "falcon_component_test_infrastructure_test",
    srcs = ["falcon_component_test_infrastructure_test.cc"],
    deps = [
        ":falcon_component_test_infrastructure",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:net_address",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/rnic:connection_manager",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/time",
        "@com_google_glog//:glog",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "falcon_testing_helpers",
    testonly = 1,
    srcs = ["falcon_testing_helpers.cc"],
    hdrs = ["falcon_testing_helpers.h"],
    deps = [
        ":connection_state_utils",
        ":event_response_format_adapter",
        ":falcon_component_interfaces",
        ":falcon_connection_state",
        ":falcon_histograms",
        "//isekai/common:config_cc_proto",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:status_util",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon/gen1:ack_coalescing_engine",
        "//isekai/host/falcon/gen1:connection_scheduler_types",
        "//isekai/host/falcon/gen1:falcon_model",
        "//isekai/host/falcon/gen1:falcon_types",
        "//isekai/host/falcon/gen1:packet_reliability_manager",
        "//isekai/host/falcon/gen1:rate_update_engine",
        "//isekai/host/falcon/gen1:rate_update_engine_adapter",
        "//isekai/host/falcon/gen2:ack_coalescing_engine",
        "//isekai/host/falcon/gen2:falcon_model",
        "//isekai/host/falcon/gen2:falcon_types",
        "//isekai/host/falcon/gen2:falcon_utils",
        "//isekai/host/falcon/rue",
        "//isekai/host/rnic:connection_manager",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest",
    ],
)

cc_library(
    name = "event_response_format_adapter",
    srcs = ["event_response_format_adapter.cc"],
    hdrs = ["event_response_format_adapter.h"],
    deps = [
        ":falcon_component_interfaces",
        ":falcon_connection_state",
        "//isekai/common:config_cc_proto",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/host/falcon",
        "//isekai/host/falcon/gen1:falcon_types",
        "//isekai/host/falcon/gen2:connection_state",
        "//isekai/host/falcon/gen2:falcon_types",
        "//isekai/host/falcon/gen2:reliability_manager",
        "//isekai/host/falcon/rue",
        "//isekai/host/falcon/rue/algorithm",
        "//isekai/host/falcon/rue/algorithm:config_cc_proto",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "connection_state_utils",
    hdrs = ["connection_state_utils.h"],
    deps = [
        "//isekai/host/falcon/gen1:falcon_types",
        "//isekai/host/falcon/gen2:falcon_types",
    ],
)

cc_library(
    name = "rue_algorithm_factories",
    hdrs = ["rue_algorithm_factories.h"],
    deps = [
        ":event_response_format_adapter",
        ":falcon_component_interfaces",
        "//isekai/common:config_cc_proto",
        "//isekai/common:status_util",
        "//isekai/host/falcon/gen1:rate_update_engine_adapter",
        "//isekai/host/falcon/rue",
        "//isekai/host/falcon/rue/algorithm",
        "//isekai/host/falcon/rue/algorithm:config_cc_proto",
        "//isekai/host/falcon/rue/algorithm:dram_state_manager",
        "//isekai/host/falcon/rue/algorithm:stateful_algorithm",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
    ],
)

cc_test(
    name = "event_response_format_adapter_test",
    srcs = ["event_response_format_adapter_test.cc"],
    deps = [
        ":event_response_format_adapter",
        ":falcon_connection_state",
        ":falcon_testing_helpers",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:packet",
        "//isekai/host/falcon",
        "//isekai/host/falcon/gen1:falcon_model",
        "//isekai/host/falcon/gen1:falcon_types",
        "//isekai/host/falcon/rue",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "falcon_bitmap_test",
    srcs = ["falcon_bitmap_test.cc"],
    deps = [
        ":falcon_bitmap",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "falcon_component_test",
    srcs = ["falcon_component_test.cc"],
    deps = [
        ":falcon_component_test_infrastructure",
        ":falcon_connection_state",
        ":falcon_counters",
        ":falcon_resource_credits",
        "//isekai/common:common_util",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon/gen1:falcon_model",
        "//isekai/host/falcon/gen1:falcon_types",
        "//isekai/host/falcon/gen1:falcon_utils",
        "//isekai/host/falcon/gen1:resource_manager",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "falcon_scheduler_queue_length_test",
    srcs = ["falcon_scheduler_queue_length_test.cc"],
    deps = [
        ":falcon_component_test_infrastructure",
        ":falcon_counters",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon/gen1:falcon_model",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "falcon",
    hdrs = ["falcon.h"],
)
