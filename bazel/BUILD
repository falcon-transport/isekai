load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake", "configure_make")

licenses(["notice"])

package(default_visibility = ["//visibility:public"])

exports_files([
    "bison.bzl",
    "flex.bzl",
    "omnetpp.bzl",
    "opp_msgc_wrapper.sh",
])

configure_make(
    name = "m4",
    autoreconf = True,
    autoreconf_options = [
        "-f",
    ],
    configure_in_place = True,
    configure_options = [
        "--disable-nls",
    ],
    lib_source = "@m4//:all",
    out_binaries = [
        "m4",
    ],
)

filegroup(
    name = "m4_bin",
    srcs = [":m4"],
    output_group = "m4",
)

configure_make(
    name = "bison",
    autoreconf = True,
    autoreconf_options = [
        "-f",
    ],
    configure_in_place = True,
    lib_source = "@bison//:all",
    out_binaries = [
        "bison",
        "yacc",
    ],
    out_static_libs = ["liby.a"],
)

filegroup(
    name = "bison_bin",
    srcs = [":bison"],
    output_group = "bison",
)

configure_make(
    name = "flex",
    lib_source = "@flex//:all",
    out_binaries = [
        "flex",
    ],
)

filegroup(
    name = "flex_bin",
    srcs = [":flex"],
    output_group = "flex",
)

configure_make(
    name = "libxml2",
    autogen = True,
    configure_in_place = True,
    configure_options = [
        "--with-zlib='no'",
        "--with-lzma='no'",
        "--without-python",
    ],
    lib_source = "@libxml//:all",
    linkopts = [
        "-ldl",
    ],
    out_include_dir = "include/libxml2",
    alwayslink = True,
)

cmake(
    name = "crc32c",
    lib_name = "libcrc32c",
    lib_source = "@com_google_crc32c//:all",
    alwayslink = True,
)
