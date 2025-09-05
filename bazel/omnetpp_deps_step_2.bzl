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
load("@rules_foreign_cc//foreign_cc:repositories.bzl", "rules_foreign_cc_dependencies")

def omnetpp_deps_step_2():
    """This function solves the external deps for omnetpp building.
    """

    rules_foreign_cc_dependencies()

    all_content = """filegroup(name = "all", srcs = glob(["**"]), visibility = ["//visibility:public"])"""
    bison_build_file_content = all_content + """
filegroup(
    name = "bison_runtime_data",
    srcs = glob(["data/**/*"]),
    output_licenses = ["unencumbered"],
    path = "data",
    visibility = ["//visibility:public"],
)
exports_files(["data"])
"""

    http_archive(
        name = "bison",
        build_file_content = bison_build_file_content,
        strip_prefix = "bison-3.6.2",
        sha256 = "e28ed3aad934de2d1df68be209ac0b454f7b6d3c3d6d01126e5cd2cbadba089a",
        urls = [
            "https://ftp.gnu.org/gnu/bison/bison-3.6.2.tar.gz",
            "https://mirrors.kernel.org/gnu/bison/bison-3.6.2.tar.gz",
        ],
    )

    http_archive(
        name = "flex",
        build_file_content = all_content,
        strip_prefix = "flex-2.6.4",
        sha256 = "e87aae032bf07c26f85ac0ed3250998c37621d95f8bd748b31f15b33c45ee995",
        urls = ["https://github.com/westes/flex/releases/download/v2.6.4/flex-2.6.4.tar.gz"],
        patches = ["//bazel:flex.patch"],
    )

    http_archive(
        name = "libxml",
        build_file_content = all_content,
        strip_prefix = "libxml2-v2.9.11",
        sha256 = "e520e37c52a93b360fd206be35fd9695389ea2c21ff885ba0dedcab5461dc306",
        urls = ["https://gitlab.gnome.org/GNOME/libxml2/-/archive/v2.9.11/libxml2-v2.9.11.tar.gz"],
    )

    if not native.existing_rule("m4"):
        http_archive(
            name = "m4",
            build_file_content = all_content,
            strip_prefix = "m4-1.4.19",
            sha256 = "3be4a26d825ffdfda52a56fc43246456989a3630093cced3fbddf4771ee58a70",
            urls = [
                "https://ftp.gnu.org/gnu/m4/m4-1.4.19.tar.gz",
                "https://mirrors.kernel.org/gnu/m4/m4-1.4.19.tar.gz",
            ],
        )
