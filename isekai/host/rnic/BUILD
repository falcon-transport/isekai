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
    name = "traffic_shaper_model",
    srcs = ["traffic_shaper_model.cc"],
    hdrs = ["traffic_shaper_model.h"],
    deps = [
        "//isekai/common:config_cc_proto",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:status_util",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/time",
        "@com_google_glog//:glog",
    ],
)

cc_library(
    name = "connection_manager",
    srcs = ["connection_manager.cc"],
    hdrs = ["connection_manager.h"],
    deps = [
        "//isekai/common:config_cc_proto",
        "//isekai/common:model_interfaces",
        "//isekai/common:status_util",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/meta:type_traits",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest_prod",
    ],
)

cc_test(
    name = "connection_manager_test",
    srcs = ["connection_manager_test.cc"],
    deps = [
        ":connection_manager",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/fabric:constants",
        "//isekai/host/falcon:falcon_connection_state",
        "//isekai/host/falcon/gen1:falcon_model",
        "//isekai/host/falcon/gen2:falcon_model",
        "//isekai/host/rdma:rdma_falcon_model",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_glog//:glog",
        "@com_google_googletest//:gtest_main",
        "@com_google_protobuf//:protobuf",
    ],
)

cc_library(
    name = "omnest_environment",
    srcs = ["omnest_environment.cc"],
    hdrs = ["omnest_environment.h"],
    deps = [
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "@com_google_absl//absl/functional:any_invocable",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_glog//:glog",
        "@omnetpp",
    ],
)

cc_library(
    name = "omnest_host",
    srcs = ["omnest_host.cc"],
    hdrs = ["omnest_host.h"],
    deps = [
        ":memory_interface",
        ":omnest_environment",
        ":omnest_packet_builder",
        ":omnest_stats_collection",
        ":rnic",
        "//isekai/common:common_util",
        "//isekai/common:config_cc_proto",
        "//isekai/common:file_util",
        "//isekai/common:model_interfaces",
        "//isekai/common:status_util",
        "//isekai/host/falcon:falcon_histograms",
        "//isekai/host/falcon:falcon_histograms_cc_proto",
        "//isekai/host/traffic:traffic_generator",
        "@com_google_absl//absl/functional:any_invocable",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_glog//:glog",
        "@inet//:omnetpp_inet",
        "@omnetpp",
    ],
)

cc_library(
    name = "omnest_stats_collection",
    srcs = ["omnest_stats_collection.cc"],
    hdrs = ["omnest_stats_collection.h"],
    deps = [
        "//isekai/common:config_cc_proto",
        "//isekai/common:model_interfaces",
        "//isekai/common:status_util",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/status",
        "@com_google_glog//:glog",
        "@omnetpp",
    ],
)

cc_library(
    name = "omnest_packet_builder",
    srcs = [
        "omnest_packet_builder.cc",
    ],
    hdrs = [
        "omnest_packet_builder.h",
        "//isekai/common:messages",
    ],
    copts = [
        "-fexceptions",
        "-Wno-private-header",
    ],
    features = ["-use_header_modules"],
    deps = [
        ":network_channel_manager",
        ":omnest_environment",
        ":traffic_shaper_model",
        "//bazel:crc32c",
        "//isekai/common:common_util",
        "//isekai/common:config_cc_proto",
        "//isekai/common:constants",
        "//isekai/common:default_config_generator",
        "//isekai/common:environment",
        "//isekai/common:file_util",
        "//isekai/common:model_interfaces",
        "//isekai/common:omnetpp_host_messages",
        "//isekai/common:packet",
        "//isekai/common:status_util",
        "//isekai/fabric:constants",
        "//isekai/fabric:memory_management_config_cc_proto",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_histograms",
        "@com_google_absl//absl/hash",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:variant",
        "@com_google_glog//:glog",
        "@com_google_googletest//:gtest_prod",
        "@inet//:omnetpp_inet",
        "@omnetpp",
    ],
)

cc_test(
    name = "omnest_packet_builder_test",
    srcs = [
        "omnest_packet_builder_test.cc",
    ],
    copts = [
        "-Wno-private-header",
    ],
    data = [
        "//isekai/testing/integration:integration_config",
        "@inet//:inet_src",
    ],
    deps = [
        ":omnest_packet_builder",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:net_address",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "//isekai/fabric:constants",
        "//isekai/fabric:packet_util",
        "//isekai/host/falcon",
        "//isekai/host/falcon/gen1:falcon_model",
        "//isekai/testing/integration:omnest_embedded_kernel",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
        "@inet//:omnetpp_inet",
        "@omnetpp",
    ],
)

cc_library(
    name = "rnic",
    srcs = ["rnic.cc"],
    hdrs = ["rnic.h"],
    deps = [
        ":memory_interface",
        "//isekai/common:model_interfaces",
        "@com_google_absl//absl/strings",
    ],
)

cc_test(
    name = "traffic_shaper_model_test",
    srcs = ["traffic_shaper_model_test.cc"],
    deps = [
        ":traffic_shaper_model",
        "//isekai/common:model_interfaces",
        "//isekai/common:packet",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
        "@omnetpp",
    ],
)

cc_test(
    name = "omnest_environment_test",
    srcs = ["omnest_environment_test.cc"],
    deps = [
        ":omnest_environment",
        "//isekai/common:model_interfaces",
        "//isekai/common:testing",
        "@com_google_absl//absl/functional:any_invocable",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/time",
        "@com_google_glog//:glog",
        "@com_google_googletest//:gtest_main",
        "@omnetpp",
    ],
)

cc_library(
    name = "memory_interface",
    srcs = ["memory_interface.cc"],
    hdrs = ["memory_interface.h"],
    deps = [
        "//isekai/common:config_cc_proto",
        "//isekai/common:environment",
        "@com_google_absl//absl/functional:any_invocable",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/time",
    ],
)

cc_library(
    name = "network_channel_manager",
    srcs = ["network_channel_manager.cc"],
    hdrs = [
        "network_channel_manager.h",
        "//isekai/common:messages",
    ],
    copts = [
        "-fexceptions",
        "-Wno-private-header",
    ],
    features = ["-use_header_modules"],
    deps = [
        ":omnest_environment",
        ":traffic_shaper_model",
        "//isekai/common:common_util",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:environment",
        "//isekai/common:file_util",
        "//isekai/common:model_interfaces",
        "//isekai/common:omnetpp_host_messages",
        "//isekai/common:packet",
        "//isekai/common:status_util",
        "//isekai/fabric:constants",
        "//isekai/fabric:memory_management_config_cc_proto",
        "//isekai/host/falcon",
        "@com_google_absl//absl/hash",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:variant",
        "@com_google_glog//:glog",
        "@com_google_googletest//:gtest_prod",
        "@inet//:omnetpp_inet",
        "@omnetpp",
        "@omnetpp//:omnetpp_headers",
    ],
)

cc_test(
    name = "memory_interface_test",
    srcs = ["memory_interface_test.cc"],
    deps = [
        ":memory_interface",
        "//isekai/common:config_cc_proto",
        "//isekai/common:simple_environment",
        "//isekai/common:testing",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)
