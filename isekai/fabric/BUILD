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
load("//bazel:omnetpp.bzl", "omnetpp_msg")

package(default_visibility = ["//isekai:__subpackages__"])

licenses(["notice"])

# Exports the Network switch configuration files like *.ned and *.ini
# so that the files can be packaged and used by flume.
exports_files(glob(["*.ned"]) + glob(["*.ini"]))

omnetpp_msg(
    name = "messages",
    srcs = glob(["*.msg"]),
)

proto_library(
    name = "routing_proto",
    srcs = ["routing.proto"],
)

cc_proto_library(
    name = "routing_cc_proto",
    deps = [":routing_proto"],
)

cc_library(
    name = "omnetpp_fabric_messages",
    srcs = [":messages"],
    copts = [
        "-fexceptions",
    ],
    features = ["-use_header_modules"],
    deps = [
        "@omnetpp",
    ],
    alwayslink = 1,
)

filegroup(
    name = "network_model_src",
    data = glob(["*"]),
)

proto_library(
    name = "memory_management_config_proto",
    srcs = ["memory_management_config.proto"],
    visibility = [
        "//isekai:__pkg__",
        "//isekai/common:__pkg__",
    ],
)

cc_proto_library(
    name = "memory_management_config_cc_proto",
    visibility = [
        "//isekai:__pkg__",
        "//isekai/host/rnic:__pkg__",
    ],
    deps = [":memory_management_config_proto"],
)

cc_library(
    name = "routing_test_util",
    testonly = 1,
    srcs = [
        "routing_test_util.cc",
    ],
    hdrs = ["routing_test_util.h"],
    copts = [
        "-Wno-private-header",
    ],
    deps = [
        ":routing_cc_proto",
        "//isekai/common:net_address",
        "//isekai/common:status_util",
        "//isekai/host/rnic:omnest_packet_builder",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_protobuf//:protobuf",
        "@com_google_riegeli//riegeli/bytes:fd_writer",
        "@com_google_riegeli//riegeli/records:record_writer",
        "@inet//:omnetpp_inet",
        "@omnetpp",
    ],
)

cc_library(
    name = "network_routing_table",
    srcs = ["network_routing_table.cc"],
    hdrs = ["network_routing_table.h"],
    deps = [
        ":packet_util",
        ":routing_cc_proto",
        "//isekai/common:file_util",
        "//isekai/common:ipv6_trie",
        "//isekai/common:net_address",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_glog//:glog",
        "@omnetpp",
    ],
)

cc_test(
    name = "network_routing_table_test",
    srcs = [
        "network_routing_table_test.cc",
    ],
    deps = [
        ":network_routing_table",
        ":routing_test_util",
        "//isekai/common:testing",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest_main",
        "@inet//:omnetpp_inet",
        "@omnetpp",
    ],
)

cc_library(
    name = "packet_util",
    srcs = [
        "packet_util.cc",
    ],
    hdrs = [
        "packet_util.h",
        ":messages",
    ],
    copts = [
        "-Wno-private-header",
    ],
    visibility = [
        "//isekai:__pkg__",
        "//isekai/host/rnic:__pkg__",
    ],
    deps = [
        ":constants",
        ":model_interfaces",
        ":omnetpp_fabric_messages",
        "//bazel:crc32c",
        "//isekai/common:common_util",
        "//isekai/common:net_address",
        "//isekai/common:packet",
        "//isekai/host/falcon",
        "//isekai/host/rnic:omnest_packet_builder",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_glog//:glog",
        "@inet//:omnetpp_inet",
        "@omnetpp",
    ],
)

cc_test(
    name = "packet_util_test",
    srcs = [
        "packet_util_test.cc",
    ],
    copts = [
        "-Wno-private-header",
    ],
    deps = [
        ":constants",
        ":packet_util",
        ":routing_test_util",
        "//bazel:crc32c",
        "//isekai/common:testing",
        "//isekai/host/rnic:omnest_packet_builder",
        "@com_google_absl//absl/hash",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest_main",
        "@inet//:omnetpp_inet",
        "@omnetpp",
    ],
)

cc_library(
    name = "port_selection",
    srcs = ["port_selection.cc"],
    hdrs = ["port_selection.h"],
    deps = [
        ":constants",
        ":memory_management_unit",
        ":network_routing_table",
        ":packet_util",
        "//isekai/common:config_cc_proto",
        "@com_google_glog//:glog",
        "@omnetpp//:omnetpp_headers",
    ],
)

cc_test(
    name = "port_selection_test",
    srcs = [
        "port_selection_test.cc",
    ],
    copts = [
        "-Wno-private-header",
    ],
    deps = [
        ":constants",
        ":network_routing_table",
        ":port_selection",
        ":routing_test_util",
        "//isekai/common:config_cc_proto",
        "//isekai/common:testing",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest_main",
        "@inet//:omnetpp_inet",
        "@omnetpp",
    ],
)

cc_library(
    name = "network_router",
    srcs = [
        "network_router.cc",
    ],
    hdrs = [
        "network_router.h",
        ":messages",
    ],
    deps = [
        ":memory_management_config_cc_proto",
        ":memory_management_unit",
        ":model_interfaces",
        ":network_port",
        ":network_routing",
        ":omnetpp_fabric_messages",
        ":packet_util",
        "//isekai/common:common_util",
        "//isekai/common:config_cc_proto",
        "//isekai/common:constants",
        "//isekai/common:default_config_generator",
        "//isekai/common:file_util",
        "//isekai/common:model_interfaces",
        "//isekai/common:status_util",
        "//isekai/host/rnic:omnest_stats_collection",
        "@com_google_absl//absl/strings",
        "@com_google_glog//:glog",
        "@omnetpp",
    ],
    # Enables alwayslink to make sure the linker does not
    # optimize away an unref library.
    alwayslink = 1,
)

cc_library(
    name = "network_port",
    srcs = ["network_port.cc"],
    hdrs = ["network_port.h"],
    deps = [
        ":arbiter",
        ":model_interfaces",
        ":network_packet_queue",
        ":packet_util",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:status_util",
        "@com_google_absl//absl/strings",
        "@com_google_glog//:glog",
        "@com_google_googletest//:gtest_prod",
        "@inet//:omnetpp_inet",
        "@omnetpp",
    ],
)

cc_library(
    name = "network_packet_queue",
    srcs = ["network_packet_queue.cc"],
    hdrs = ["network_packet_queue.h"],
    deps = [
        ":model_interfaces",
        ":packet_util",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:status_util",
        "@com_google_absl//absl/strings",
        "@com_google_glog//:glog",
        "@inet//:omnetpp_inet",
        "@omnetpp",
    ],
)

cc_library(
    name = "network_routing",
    srcs = [
        "network_routing.cc",
    ],
    hdrs = [
        "network_routing.h",
    ],
    copts = [
        "-Wno-private-header",
    ],
    deps = [
        ":constants",
        ":model_interfaces",
        ":network_routing_table",
        ":packet_util",
        ":port_selection",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:status_util",
        "@com_google_googletest//:gtest_prod",
        "@inet//:omnetpp_inet",
        "@omnetpp",
    ],
)

cc_test(
    name = "network_routing_test",
    srcs = [
        "network_routing_test.cc",
    ],
    data = [
        "//isekai/test_data:test_configurations",
    ],
    deps = [
        ":network_router",
        ":network_routing",
        ":packet_util",
        ":routing_test_util",
        "//isekai/common:config_cc_proto",
        "//isekai/common:net_address",
        "//isekai/common:testing",
        "@com_google_absl//absl/status:statusor",
        "@com_google_googletest//:gtest_main",
        "@inet//:omnetpp_inet",
    ],
)

cc_library(
    name = "arbiter",
    srcs = [
        "arbiter.cc",
    ],
    hdrs = ["arbiter.h"],
    deps = [
        ":model_interfaces",
        "//isekai/common:config_cc_proto",
        "@com_google_absl//absl/strings",
        "@com_google_glog//:glog",
        "@omnetpp",
    ],
)

cc_test(
    name = "arbiter_test",
    srcs = [
        "arbiter_test.cc",
    ],
    deps = [
        ":arbiter",
        ":routing_test_util",
        "//isekai/common:config_cc_proto",
        "@com_google_googletest//:gtest_main",
        "@omnetpp",
    ],
)

cc_library(
    name = "weighted_random_early_detection",
    srcs = [
        "weighted_random_early_detection.cc",
    ],
    hdrs = ["weighted_random_early_detection.h"],
    deps = [
        ":memory_management_config_cc_proto",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/random:distributions",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_glog//:glog",
        "@omnetpp",
    ],
)

cc_test(
    name = "weighted_random_early_detection_test",
    srcs = [
        "weighted_random_early_detection_test.cc",
    ],
    data = [
        "//isekai/test_data:test_configurations",
    ],
    deps = [
        ":memory_management_config_cc_proto",
        ":weighted_random_early_detection",
        "@com_google_absl//absl/strings",
        "@com_google_glog//:glog",
        "@com_google_googletest//:gtest_main",
        "@com_google_protobuf//:protobuf",
    ],
)

cc_library(
    name = "memory_management_unit",
    srcs = [
        "memory_management_unit.cc",
    ],
    hdrs = ["memory_management_unit.h"],
    copts = [
        "-Wno-private-header",
    ],
    deps = [
        ":constants",
        ":memory_management_config_cc_proto",
        ":model_interfaces",
        ":packet_util",
        ":weighted_random_early_detection",
        "//isekai/common:bitmap",
        "//isekai/common:config_cc_proto",
        "//isekai/common:default_config_generator",
        "//isekai/common:model_interfaces",
        "//isekai/common:status_util",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:optional",
        "@com_google_glog//:glog",
        "@com_google_googletest//:gtest_prod",
        "@inet//:omnetpp_inet",
        "@omnetpp",
    ],
)

cc_test(
    name = "memory_management_unit_test",
    srcs = ["memory_management_unit_test.cc"],
    copts = [
        "-Wno-private-header",
    ],
    data = [
        "//isekai/test_data:test_configurations",
    ],
    deps = [
        ":constants",
        ":memory_management_unit",
        ":network_packet_queue",
        ":network_port",
        ":network_router",
        ":packet_util",
        ":routing_test_util",
        "//isekai/common:config_cc_proto",
        "//isekai/common:file_util",
        "//isekai/common:testing",
        "//isekai/host/rnic:omnest_packet_builder",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest_main",
        "@inet//:omnetpp_inet",
        "@omnetpp",
    ],
)

cc_library(
    name = "model_interfaces",
    hdrs = [
        "model_interfaces.h",
        ":messages",
    ],
    deps = [
        ":constants",
        ":omnetpp_fabric_messages",
        "@com_google_absl//absl/time",
        "@omnetpp",
    ],
)

cc_library(
    name = "constants",
    hdrs = ["constants.h"],
    visibility = [
        "//isekai:__subpackages__",
    ],
    deps = ["//isekai/common:net_address"],
    alwayslink = 1,
)
