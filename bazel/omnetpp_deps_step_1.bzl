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

"""OMNeT++ deps rules"""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("//bazel:omnetpp_bzl_version.bzl", "omnetpp_bazel_version")

def omnetpp_deps_step_1():
    """Uses rules_foreign_cc to build the external deps of omnetpp.
    """

    omnetpp_bazel_version()

    if not native.existing_rule("rules_foreign_cc"):
        http_archive(
            name = "rules_foreign_cc",
            sha256 = "476303bd0f1b04cc311fc258f1708a5f6ef82d3091e53fd1977fa20383425a6a",
            strip_prefix = "rules_foreign_cc-0.10.1",
            url = "https://github.com/bazelbuild/rules_foreign_cc/releases/download/0.10.1/rules_foreign_cc-0.10.1.tar.gz",
        )
