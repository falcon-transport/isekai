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

def omnetpp_deps_step_3():
    #omnetpp
    http_archive(
        name = "omnetpp",
        strip_prefix = "omnetpp-5.6.1",
        sha256 = "7f17ec61c3a6934948ff241b7167e27e683200e9498bbe5315413e4fbf48cbc1",
        urls = [
            "https://github.com/omnetpp/omnetpp/releases/download/omnetpp-5.6.1/omnetpp-5.6.1-src-linux.tgz",
        ],
        build_file = "@//bazel:omnetpp.BUILD",
        patches = ["//bazel:omnetpp.patch"],
    )

    #INET
    http_archive(
        name = "inet",
        strip_prefix = "inet-4.1.1",
        sha256 = "638b2a77e9638db5061fc6f811048a5abde9592baea02b352dbab13d24f4b8f9",
        urls = [
            "https://github.com/inet-framework/inet/archive/refs/tags/v4.1.1.tar.gz",
        ],
        build_file = "@//bazel:inet.BUILD",
        patches = ["//bazel:inet.patch"],
    )
