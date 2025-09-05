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
    name = "rdma_base_model",
    srcs = ["rdma_base_model.cc"],
    hdrs = ["rdma_base_model.h"],
    deps = [
        ":rdma_component_interfaces",
        ":rdma_latency_histograms_cc_proto",
        ":rdma_qp_manager",
        "//isekai/common:common_util",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:net_address",
        "//isekai/common:status_util",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_glog//:glog",
    ],
)

cc_library(
    name = "rdma_component_interfaces",
    hdrs = ["rdma_component_interfaces.h"],
    deps = [
        "//isekai/common:model_interfaces",
        "//isekai/common:net_address",
        "//isekai/common:packet",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/functional:any_invocable",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/time",
    ],
)

cc_library(
    name = "rdma_falcon_model",
    srcs = [
        "rdma_falcon_model.cc",
        "rdma_falcon_work_scheduler.cc",
        "rdma_falcon_work_scheduler.h",
    ],
    hdrs = ["rdma_falcon_model.h"],
    deps = [
        ":rdma_base_model",
        ":rdma_component_interfaces",
        ":rdma_latency_histograms_cc_proto",
        ":rdma_per_host_rx_buffers",
        ":rdma_qp_manager",
        "//isekai/common:common_util",
        "//isekai/common:config_cc_proto",
        "//isekai/common:constants",
        "//isekai/common:default_config_generator",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:tdigest",
        "//isekai/common:token_bucket",
        "//isekai/host/falcon/gen1:falcon_utils",
        "//isekai/host/rnic:memory_interface",
        "@com_google_absl//absl/algorithm:container",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_glog//:glog",
    ],
)

cc_test(
    name = "rdma_falcon_model_test",
    srcs = ["rdma_falcon_model_test.cc"],
    deps = [
        ":rdma_component_interfaces",
        ":rdma_falcon_model",
        "//isekai/common:common_util",
        "//isekai/common:config_cc_proto",
        "//isekai/common:constants",
        "//isekai/common:default_config_generator",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/host/falcon:falcon_histograms",
        "//isekai/host/rnic:memory_interface",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

proto_library(
    name = "rdma_latency_histograms_proto",
    srcs = ["rdma_latency_histograms.proto"],
    deps = ["//isekai/common:tdigest_proto"],
)

cc_proto_library(
    name = "rdma_latency_histograms_cc_proto",
    deps = [":rdma_latency_histograms_proto"],
)

cc_library(
    name = "rdma_qp_manager",
    srcs = ["rdma_qp_manager.cc"],
    hdrs = ["rdma_qp_manager.h"],
    deps = [
        ":rdma_component_interfaces",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/functional:any_invocable",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/time",
    ],
)

cc_library(
    name = "rdma_per_host_rx_buffers",
    srcs = ["rdma_per_host_rx_buffers.cc"],
    hdrs = ["rdma_per_host_rx_buffers.h"],
    deps = [
        "//isekai/common:config_cc_proto",
        "//isekai/common:constants",
        "//isekai/common:model_interfaces",
        "//isekai/host/rnic:memory_interface",
        "@com_google_absl//absl/functional:any_invocable",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
    ],
)

cc_test(
    name = "rdma_per_host_rx_buffers_test",
    srcs = ["rdma_per_host_rx_buffers_test.cc"],
    deps = [
        ":rdma_per_host_rx_buffers",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/host/falcon:falcon_histograms",
        "//isekai/host/rnic:memory_interface",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)
