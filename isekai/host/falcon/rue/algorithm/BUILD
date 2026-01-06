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

package(default_visibility = ["//visibility:public"])

proto_library(
    name = "config_proto",
    srcs = [
        "algorithm.proto",
        "bypass.proto",
        "swift.proto",
    ],
)

cc_proto_library(
    name = "config_cc_proto",
    deps = [":config_proto"],
)

cc_library(
    name = "algorithm",
    srcs = [
        "bypass.cc",
        "swift.cc",
    ],
    hdrs = [
        "bypass.h",
        "connection_state.h",
        "hardware_state.h",
        "swift.h",
    ],
    deps = [
        ":config_cc_proto",
        "//isekai/common:status_util",
        "//isekai/host/falcon",
        "//isekai/host/falcon/rue",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/time",
    ],
)

cc_library(
    name = "dram_state_manager",
    hdrs = [
        "dram_state_manager.h",
    ],
    deps = [
        "//isekai/host/falcon/rue",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/log:check",
    ],
)

cc_test(
    name = "dram_state_manager_test",
    srcs = [
        "dram_state_manager_test.cc",
    ],
    deps = [
        ":dram_state_manager",
        "//isekai/common:testing",
        "//isekai/host/falcon/rue",
        "@com_google_absl//absl/log:check",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "stateful_algorithm",
    hdrs = [
        "stateful_algorithm.h",
    ],
    deps = [
        ":algorithm",
        ":dram_state_manager",
        "//isekai/host/falcon/rue",
    ],
)

cc_test(
    name = "stateful_algorithm_test",
    srcs = [
        "stateful_algorithm_test.cc",
    ],
    deps = [
        ":algorithm",
        ":config_cc_proto",
        ":dram_state_manager",
        ":stateful_algorithm",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon/rue",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "bypass_test",
    srcs = [
        "bypass_test.cc",
    ],
    deps = [
        ":algorithm",
        ":config_cc_proto",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon/rue",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "swift_test",
    srcs = [
        "swift_test.cc",
    ],
    deps = [
        ":algorithm",
        ":config_cc_proto",
        ":dram_state_manager",
        ":stateful_algorithm",
        "//isekai/common:status_util",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon/rue",
        "//isekai/host/falcon/rue:state_manager",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/status",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "swift_gen2_test",
    srcs = ["swift_gen2_test.cc"],
    deps = [
        ":algorithm",
        ":config_cc_proto",
        ":dram_state_manager",
        ":stateful_algorithm",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon/rue",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/status",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "swift_gen3_test",
    srcs = ["swift_gen3_test.cc"],
    deps = [
        ":algorithm",
        ":config_cc_proto",
        ":dram_state_manager",
        ":stateful_algorithm",
        "//isekai/common:testing",
        "//isekai/host/falcon",
        "//isekai/host/falcon/rue",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/status",
        "@com_google_googletest//:gtest_main",
    ],
)
