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

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "rue",
    hdrs = [
        "bits.h",
        "fixed.h",
        "format.h",
        "format_gen1.h",
        "format_gen2.h",
        "format_gen3.h",
        "util.h",
    ],
    deps = [
        "//isekai/host/falcon",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/log:check",
    ],
)

cc_library(
    name = "latency_generator",
    hdrs = ["latency_generator.h"],
    deps = [
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/random:bit_gen_ref",
        "@com_google_absl//absl/random:distributions",
        "@com_google_absl//absl/time",
    ],
)

cc_library(
    name = "state_manager",
    srcs = ["state_manager.h"],
    deps = [
        ":rue",
        "//isekai/host/falcon/rue/algorithm",
        "//isekai/host/falcon/rue/algorithm:dram_state_manager",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/time",
    ],
)

cc_test(
    name = "bits_test",
    srcs = [
        "bits_test.cc",
    ],
    deps = [
        ":rue",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "fixed_test",
    srcs = [
        "fixed_test.cc",
    ],
    deps = [
        ":rue",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "format_test",
    srcs = [
        "format_test.cc",
    ],
    deps = [
        ":rue",
        "//isekai/host/falcon",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "util_test",
    srcs = [
        "util_test.cc",
    ],
    deps = [
        ":rue",
        "//isekai/host/falcon",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "latency_generator_test",
    srcs = ["latency_generator_test.cc"],
    deps = [
        ":latency_generator",
        "@com_google_absl//absl/random:mock_distributions",
        "@com_google_absl//absl/random:mocking_bit_gen",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)
