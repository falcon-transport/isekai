licenses(["notice"])

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "folly",
    srcs = glob(
        [
            "folly/stats/*.cpp",
            "folly/stats/detail/*.cpp",
        ],
    ),
    hdrs = glob(
        ["folly/**/*.h"],
        exclude = ["folly/**/test/**/*.h"],
    ),
    defines = [
        "FOLLY_NO_CONFIG",
        "FOLLY_USE_LIBCPP=1",
        "FOLLY_HAVE_LIBJEMALLOC=0",
    ],
    features = [
        "-layering_check",
    ],
    includes = ["."],
    deps = [
        "@boost//:graph",
        "@com_google_glog//:glog",
    ],
    alwayslink = True,
)
