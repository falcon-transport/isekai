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

omnetpp_msg(
    name = "messages",
    srcs = glob(["*.msg"]),
)

proto_library(
    name = "config_proto",
    srcs = ["config.proto"],
    deps = ["//isekai/fabric:memory_management_config_proto"],
)

cc_proto_library(
    name = "config_cc_proto",
    deps = [":config_proto"],
)

cc_library(
    name = "omnetpp_host_messages",
    srcs = [":messages"],
    # The automatically generated cpp files for the
    # OMNest messages use C++ exceptions.
    copts = [
        "-fexceptions",
    ],
    features = ["-use_header_modules"],
    deps = [
        ":packet",
        "@inet//:omnetpp_inet",
        "@omnetpp",
    ],
    alwayslink = 1,
)

cc_library(
    name = "environment",
    hdrs = ["environment.h"],
    deps = [
        "@com_google_absl//absl/functional:any_invocable",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/time",
    ],
)

cc_library(
    name = "constants",
    hdrs = ["constants.h"],
    deps = [],
)

cc_library(
    name = "packet",
    srcs = ["packet.cc"],
    hdrs = ["packet.h"],
    deps = [
        ":constants",
        "//isekai/common:net_address",
        "//isekai/host/falcon",
        "//isekai/host/falcon:falcon_bitmap",
        "//isekai/host/falcon/rue",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
    ],
)

cc_library(
    name = "simple_environment",
    testonly = 1,
    srcs = ["simple_environment.cc"],
    hdrs = ["simple_environment.h"],
    deps = [
        ":environment",
        "@com_google_absl//absl/functional:any_invocable",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/time",
        "@com_google_glog//:glog",
    ],
)

cc_library(
    name = "model_interfaces",
    hdrs = ["model_interfaces.h"],
    deps = [
        ":config_cc_proto",
        ":environment",
        ":packet",
        "//isekai/host/falcon:falcon_histograms",
        "//isekai/host/falcon:falcon_resource_credits",
        "//isekai/host/rdma:rdma_latency_histograms_cc_proto",
        "@com_google_absl//absl/functional:any_invocable",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@omnetpp",
    ],
)

cc_test(
    name = "packet_test",
    srcs = ["packet_test.cc"],
    deps = [
        ":packet",
        "//isekai/common:net_address",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "simple_environment_test",
    srcs = ["simple_environment_test.cc"],
    deps = [
        ":simple_environment",
        "//isekai/common:testing",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "token_bucket",
    srcs = ["token_bucket.cc"],
    hdrs = ["token_bucket.h"],
    deps = [
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/time",
    ],
)

cc_test(
    name = "token_bucket_test",
    srcs = ["token_bucket_test.cc"],
    deps = [
        ":token_bucket",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "common_util",
    srcs = ["common_util.cc"],
    hdrs = ["common_util.h"],
    deps = [
        ":config_cc_proto",
        "@com_google_absl//absl/strings",
        "@com_google_glog//:glog",
    ],
)

cc_library(
    name = "status_util",
    srcs = ["status_util.cc"],
    hdrs = ["status_util.h"],
    deps = [
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_glog//:glog",
    ],
)

cc_test(
    name = "status_util_test",
    srcs = ["status_util_test.cc"],
    deps = [
        ":status_util",
        "@com_google_absl//absl/status",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "default_config_generator",
    srcs = ["default_config_generator.cc"],
    hdrs = ["default_config_generator.h"],
    deps = [
        ":config_cc_proto",
        ":constants",
        "//isekai/host/falcon/rue/algorithm",
        "@com_google_absl//absl/log",
    ],
)

proto_library(
    name = "tdigest_proto",
    srcs = ["tdigest.proto"],
)

cc_proto_library(
    name = "tdigest_cc_proto",
    deps = [":tdigest_proto"],
)

cc_library(
    name = "tdigest",
    srcs = [
        "tdigest.cc",
    ],
    hdrs = [
        "tdigest.h",
    ],
    deps = [
        ":tdigest_cc_proto",
        "@folly",
    ],
)

cc_library(
    name = "file_util",
    srcs = [
        "file_util.cc",
    ],
    hdrs = [
        "file_util.h",
    ],
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_glog//:glog",
        "@com_google_protobuf//:protobuf",
        "@com_google_riegeli//riegeli/bytes:fd_reader",
        "@com_google_riegeli//riegeli/bytes:fd_writer",
        "@com_google_riegeli//riegeli/messages:text_parse_message",
        "@com_google_riegeli//riegeli/records:record_reader",
        "@com_google_riegeli//riegeli/records:record_writer",
    ],
)

cc_library(
    name = "ipv6_trie",
    srcs = [
        "ipv6_trie.cc",
    ],
    hdrs = [
        "ipv6_trie.h",
    ],
    deps = [
        "@com_google_absl//absl/numeric:int128",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_glog//:glog",
    ],
)

cc_test(
    name = "ipv6_trie_test",
    srcs = ["ipv6_trie_test.cc"],
    deps = [
        ":ipv6_trie",
        "//isekai/common:testing",
        "@com_google_absl//absl/numeric:int128",
        "@com_google_absl//absl/status:statusor",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "file_util_test",
    srcs = ["file_util_test.cc"],
    data = [
        "//isekai/test_data:test_configurations",
    ],
    deps = [
        ":file_util",
        ":testing",
        "//isekai/common:config_cc_proto",
        "@com_google_absl//absl/flags:flag",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "tdigest_test",
    srcs = ["tdigest_test.cc"],
    deps = [
        ":tdigest",
        ":tdigest_cc_proto",
        "@com_google_glog//:glog",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "net_address",
    srcs = [
        "net_address.cc",
    ],
    hdrs = [
        "net_address.h",
    ],
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_glog//:glog",
    ],
)

cc_test(
    name = "net_address_test",
    srcs = ["net_address_test.cc"],
    deps = [
        ":net_address",
        "//isekai/common:testing",
        "@com_google_absl//absl/status",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "bitmap",
    srcs = [
        "bitmap.cc",
    ],
    hdrs = [
        "bitmap.h",
    ],
    deps = ["@com_google_glog//:glog"],
)

cc_test(
    name = "bitmap_test",
    srcs = ["bitmap_test.cc"],
    deps = [
        ":bitmap",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "testing",
    hdrs = [
        "testing.h",
    ],
    deps = [
        "//isekai/common:status_util",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_googletest//:gtest",
    ],
)
