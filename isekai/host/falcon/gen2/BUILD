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
    name = "connection_state_manager",
    srcs = ["connection_state_manager.cc"],
    hdrs = ["connection_state_manager.h"],
    deps = [
        ":connection_state",
        ":falcon_types",
        ":falcon_utils",
        "//isekai/common:constants",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon/gen1:connection_state_manager",
        "//isekai/host/falcon/gen1:falcon_types",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/status",
    ],
)

cc_test(
    name = "connection_state_manager_test",
    srcs = ["connection_state_manager_test.cc"],
    deps = [
        ":connection_state_manager",
        ":falcon_model",
        ":falcon_types",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_testing_helpers",
        "//isekai/host/falcon/gen1:falcon_model",
        "//isekai/host/rnic:connection_manager",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/status",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "resource_manager",
    srcs = ["resource_manager.cc"],
    hdrs = ["resource_manager.h"],
    deps = [
        ":falcon_component_interfaces",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:status_util",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon/gen1:falcon_types",
        "//isekai/host/falcon/gen1:resource_manager",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
    ],
)

cc_library(
    name = "reliability_manager",
    srcs = ["reliability_manager.cc"],
    hdrs = ["reliability_manager.h"],
    deps = [
        ":falcon_types",
        ":falcon_utils",
        "//isekai/common:config_cc_proto",
        "//isekai/common:constants",
        "//isekai/common:packet",
        "//isekai/common:status_util",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon/gen1:connection_scheduler_types",
        "//isekai/host/falcon/gen1:falcon_types",
        "//isekai/host/falcon/gen1:falcon_utils",
        "//isekai/host/falcon/gen1:packet_reliability_manager",
        "//isekai/host/falcon/gen1:weighted_round_robin_policy",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/time",
    ],
)

cc_test(
    name = "reliability_manager_test",
    srcs = ["reliability_manager_test.cc"],
    deps = [
        ":falcon_types",
        ":falcon_utils",
        ":reliability_manager",
        "//isekai/common:config_cc_proto",
        "//isekai/common:constants",
        "//isekai/common:default_config_generator",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon:connection_state_utils",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_testing_helpers",
        "//isekai/host/falcon/gen1:rate_update_engine",
        "//isekai/host/falcon/rue",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
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
        ":falcon_utils",
        "//isekai/common:config_cc_proto",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:status_util",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon/gen1:falcon_types",
        "//isekai/host/falcon/gen1:rate_update_engine",
        "//isekai/host/falcon/gen1:rate_update_engine_adapter",
        "//isekai/host/falcon/rue",
        "//isekai/host/falcon/rue/algorithm",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/numeric:bits",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
    ],
)

cc_test(
    name = "rate_update_engine_test",
    srcs = ["rate_update_engine_test.cc"],
    deps = [
        ":falcon_types",
        ":falcon_utils",
        ":rate_update_engine",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_counters",
        "//isekai/host/falcon:falcon_testing_helpers",
        "//isekai/host/falcon/gen1:falcon_model",
        "//isekai/host/falcon/gen1:falcon_types",
        "//isekai/host/falcon/gen1:rate_update_engine",
        "//isekai/host/falcon/rue",
        "//isekai/host/rnic:connection_manager",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "falcon_model",
    srcs = ["falcon_model.cc"],
    hdrs = ["falcon_model.h"],
    deps = [
        ":connection_state",
        ":falcon_component_interfaces",
        ":falcon_types",
        ":falcon_utils",
        "//isekai/common:config_cc_proto",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:status_util",
        "//isekai/host/falcon:falcon_component_factories",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon/gen1:falcon_model",
        "//isekai/host/falcon/gen1:falcon_types",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "ack_coalescing_engine",
    srcs = ["ack_coalescing_engine.cc"],
    hdrs = ["ack_coalescing_engine.h"],
    deps = [
        ":falcon_types",
        ":falcon_utils",
        "//isekai/common:constants",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:status_util",
        "//isekai/host/falcon:falcon_bitmap",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon/gen1:ack_coalescing_engine",
        "//isekai/host/falcon/gen1:falcon_types",
        "//isekai/host/falcon/gen1:falcon_utils",
    ],
)

cc_test(
    name = "ack_coalescing_engine_test",
    srcs = ["ack_coalescing_engine_test.cc"],
    deps = [
        ":ack_coalescing_engine",
        "//isekai/common:config_cc_proto",
        "//isekai/common:constants",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_bitmap",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_testing_helpers",
        "//isekai/host/falcon/gen1:ack_coalescing_engine",
        "//isekai/host/falcon/gen1:falcon_model",
        "//isekai/host/falcon/gen1:falcon_types",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "event_response_format_adapter_test",
    srcs = ["event_response_format_adapter_test.cc"],
    deps = [
        ":connection_state",
        ":falcon_types",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:packet",
        "//isekai/host/falcon",
        "//isekai/host/falcon:event_response_format_adapter",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_testing_helpers",
        "//isekai/host/falcon/gen1:falcon_model",
        "//isekai/host/falcon/gen1:falcon_types",
        "//isekai/host/falcon/rue",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "falcon_utils",
    hdrs = ["falcon_utils.h"],
    deps = [
        ":falcon_types",
        "//isekai/common:status_util",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon/gen1:falcon_types",
        "//isekai/host/falcon/gen1:falcon_utils",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
    ],
)

cc_library(
    name = "falcon_types",
    hdrs = ["falcon_types.h"],
    deps = [
        "//isekai/common:model_interfaces",
        "//isekai/host/falcon",
        "//isekai/host/falcon/gen1:falcon_types",
        "//isekai/host/falcon/rue",
        "@com_google_absl//absl/time",
    ],
)

cc_library(
    name = "falcon_component_interfaces",
    hdrs = ["falcon_component_interfaces.h"],
    deps = [
        "//isekai/common:model_interfaces",
        "//isekai/host/rnic:memory_interface",
        "@com_google_absl//absl/status",
    ],
)

cc_library(
    name = "ulp_backpressure_manager",
    srcs = ["ulp_backpressure_manager.cc"],
    hdrs = ["ulp_backpressure_manager.h"],
    deps = [
        ":connection_state",
        ":falcon_component_interfaces",
        ":ulp_backpressure_policy_alpha_carving",
        "//isekai/common:model_interfaces",
        "//isekai/common:status_util",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon/gen1:falcon_types",
        "@com_google_absl//absl/status",
        "@com_google_glog//:glog",
    ],
)

cc_test(
    name = "ulp_backpressure_manager_test",
    srcs = ["ulp_backpressure_manager_test.cc"],
    deps = [
        ":connection_state",
        ":falcon_model",
        ":falcon_types",
        "//isekai/common:config_cc_proto",
        "//isekai/common:constants",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_counters",
        "//isekai/host/rnic:connection_manager",
        "@com_google_absl//absl/algorithm:container",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "ulp_backpressure_policy_alpha_carving",
    srcs = ["ulp_backpressure_policy_alpha_carving.cc"],
    hdrs = ["ulp_backpressure_policy_alpha_carving.h"],
    deps = [
        ":connection_state",
        ":falcon_component_interfaces",
        "//isekai/common:model_interfaces",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon:falcon_counters",
        "//isekai/host/falcon:falcon_resource_credits",
        "//isekai/host/falcon/rue/algorithm",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
    ],
)

cc_test(
    name = "ulp_backpressure_policy_alpha_carving_test",
    srcs = ["ulp_backpressure_policy_alpha_carving_test.cc"],
    deps = [
        ":connection_state",
        ":falcon_types",
        ":ulp_backpressure_policy_alpha_carving",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon/gen1:falcon_model",
        "//isekai/host/rnic:connection_manager",
        "@com_google_absl//absl/status:statusor",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "packet_metadata_transformer",
    srcs = ["packet_metadata_transformer.cc"],
    hdrs = ["packet_metadata_transformer.h"],
    deps = [
        ":falcon_utils",
        "//isekai/common:packet",
        "//isekai/host/falcon:falcon_component_interfaces",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon/gen1:packet_metadata_transformer",
        "@com_google_absl//absl/log:check",
    ],
)

cc_library(
    name = "connection_state",
    hdrs = ["connection_state.h"],
    deps = [
        ":falcon_types",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon/gen1:falcon_types",
    ],
)

cc_test(
    name = "packet_metadata_transformer_test",
    srcs = ["packet_metadata_transformer_test.cc"],
    deps = [
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
        "//isekai/host/falcon/gen1:falcon_types",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)
