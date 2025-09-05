load("@//bazel:omnetpp_deps_step_1.bzl", "omnetpp_deps_step_1")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

omnetpp_deps_step_1()

load("@//bazel:omnetpp_deps_step_2.bzl", "omnetpp_deps_step_2")

omnetpp_deps_step_2()

load("@//bazel:omnetpp_deps_step_3.bzl", "omnetpp_deps_step_3")

omnetpp_deps_step_3()

# Isekai external deps
all_content = """filegroup(name = "all", srcs = glob(["**"]), visibility = ["//visibility:public"])"""

# rules_cc
http_archive(
    name = "rules_cc",
    sha256 = "0d3b4f984c4c2e1acfd1378e0148d35caf2ef1d9eb95b688f8e19ce0c41bdf5b",
    strip_prefix = "rules_cc-0.1.4",
    url = "https://github.com/bazelbuild/rules_cc/releases/download/0.1.4/rules_cc-0.1.4.tar.gz",
)

# glog
http_archive(
    name = "com_github_gflags_gflags",
    sha256 = "34af2f15cf7367513b352bdcd2493ab14ce43692d2dcd9dfc499492966c64dcf",
    strip_prefix = "gflags-2.2.2",
    urls = ["https://github.com/gflags/gflags/archive/v2.2.2.tar.gz"],
)

http_archive(
    name = "com_google_glog",
    sha256 = "21bc744fb7f2fa701ee8db339ded7dce4f975d0d55837a97be7d46e8382dea5a",
    strip_prefix = "glog-0.5.0",
    urls = ["https://github.com/google/glog/archive/v0.5.0.zip"],
)

#crc32
http_archive(
    name = "com_google_crc32c",
    build_file_content = all_content,
    patches = ["//bazel:crc32c.patch"],
    sha256 = "ac07840513072b7fcebda6e821068aa04889018f24e10e46181068fb214d7e56",
    strip_prefix = "crc32c-1.1.2",
    urls = ["https://github.com/google/crc32c/archive/refs/tags/1.1.2.tar.gz"],
)

#folly
http_archive(
    name = "com_github_nelhage_rules_boost",
    integrity = "sha256-F4aAsKUJPhenR7/ohgrcfVwyqRBZg8EHVHghCXPRsIM=",
    strip_prefix = "rules_boost-d8f4f9f88b12461b19354dea4df5f9ee78262067",
    url = "https://github.com/nelhage/rules_boost/archive/d8f4f9f88b12461b19354dea4df5f9ee78262067.tar.gz",
)

load("@com_github_nelhage_rules_boost//:boost/boost.bzl", "boost_deps")

boost_deps()

http_archive(
    name = "folly",
    build_file = "@//bazel:folly.BUILD",
    integrity = "sha256-1QQY0On7Yg3vNv61DEqMYN1NfcOl7xrPSGck8OeluD4=",
    strip_prefix = "folly-2024.05.06.00",
    url = "https://github.com/facebook/folly/archive/refs/tags/v2024.05.06.00.tar.gz",
)

#protobuf
http_archive(
    name = "com_google_protobuf",
    sha256 = "7c3ebd7aaedd86fa5dc479a0fda803f602caaf78d8aff7ce83b89e1b8ae7442a",
    strip_prefix = "protobuf-28.3",
    urls = ["https://github.com/protocolbuffers/protobuf/releases/download/v28.3/protobuf-28.3.tar.gz"],
)

load("@com_google_protobuf//:protobuf_deps.bzl", "protobuf_deps")

protobuf_deps()

#googletest
http_archive(
    name = "com_google_googletest",
    sha256 = "b4870bf121ff7795ba20d20bcdd8627b8e088f2d1dab299a031c1034eddc93d5",
    strip_prefix = "googletest-release-1.11.0",
    urls = [
        "https://github.com/google/googletest/archive/refs/tags/release-1.11.0.tar.gz",
    ],
)

#riegeli
http_archive(
    name = "com_google_riegeli",
    sha256 = "04b16b65b75bb7b307a72c8aa5e49cf67b5f0731d683042dab7aeeac54adaf50",
    strip_prefix = "riegeli-c4d1f275ed44db839385e494c3a969ae232d6e10",
    url = "https://github.com/google/riegeli/archive/c4d1f275ed44db839385e494c3a969ae232d6e10.zip",
)

#external deps for riegeli
http_archive(
    name = "highwayhash",
    build_file = "//isekai:highwayhash.BUILD",
    sha256 = "cf891e024699c82aabce528a024adbe16e529f2b4e57f954455e0bf53efae585",
    strip_prefix = "highwayhash-276dd7b4b6d330e4734b756e97ccfb1b69cc2e12",
    urls = ["https://github.com/google/highwayhash/archive/276dd7b4b6d330e4734b756e97ccfb1b69cc2e12.zip"],
)

http_archive(
    name = "org_brotli",
    sha256 = "2b8ad5cf2772a9cff714fe3b8de8366c42f72a560921c2235faab0647faf5270",
    strip_prefix = "brotli-271be114299d844f3834325422402449afb0a2c7",
    urls = ["https://github.com/google/brotli/archive/271be114299d844f3834325422402449afb0a2c7.zip"],
)

http_archive(
    name = "net_zstd",
    build_file = "//isekai:zstd.BUILD",
    sha256 = "8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1",
    strip_prefix = "zstd-1.5.6",
    urls = ["https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz"],
)

http_archive(
    name = "snappy",
    sha256 = "736aeb64d86566d2236ddffa2865ee5d7a82d26c9016b36218fcc27ea4f09f86",
    strip_prefix = "snappy-1.2.1",
    urls = ["https://github.com/google/snappy/archive/refs/tags/1.2.1.tar.gz"],
)

# cel-cpp
http_archive(
    name = "com_google_cel_cpp",
    sha256 = "67fafc14e523c75e74b1706d24e5b064f1eeefb9fe3a10704709343728ac07e6",
    strip_prefix = "cel-cpp-0.7.0",
    url = "https://github.com/google/cel-cpp/archive/refs/tags/v0.7.0.tar.gz",
)
