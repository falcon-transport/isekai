load("@//bazel:omnetpp.bzl", "omnetpp_msg")

package(default_visibility = ["//visibility:public"])

exports_files(glob(["src/**/*"]))

# Source directories that are not build.
EXCLUDED_SOURCE_DIRS = [
    "src/inet/applications/netperfmeter/**",
    "src/inet/applications/voip/**",
    "src/inet/applications/voipstream/**",
    "src/inet/transportlayer/tcp_lwip/**",
    "src/inet/transportlayer/tcp_nsc/**",
    "src/inet/visualizer/**",
]

COPTS = [
    "-DWITH_ACKINGWIRELESS",
    "-DWITH_AODV",
    "-DWITH_APSKRADIO",
    "-DWITH_BGPv4",
    "-DWITH_BMAC",
    "-DWITH_CSMACA",
    "-DWITH_DHCP",
    "-DWITH_DSDV",
    "-DWITH_DYMO",
    "-DWITH_ETHERNET",
    "-DWITH_FLOODING",
    "-DWITH_IEEE80211",
    "-DWITH_IEEE802154",
    "-DWITH_IEEE8021D",
    "-DWITH_IPv4",
    "-DWITH_IPv6",
    "-DWITH_LMAC",
    "-DWITH_MPLS",
    "-DWITH_NEXTHOP",
    "-DWITH_OSPFv2",
    "-DWITH_PACKETDRILL",
    "-DWITH_PIM",
    "-DWITH_POWER",
    "-DWITH_PPP",
    "-DWITH_RADIO",
    "-DWITH_RIP",
    "-DWITH_RTP",
    "-DWITH_SCTP",
    "-DWITH_SHORTCUT",
    "-DWITH_TCP_COMMON",
    "-DWITH_TCP_INET",
    "-DWITH_TUN",
    "-DWITH_UDP",
    "-DWITH_XMAC",
    "-DWITH_xMIPv6",
    "-DWITH_GOOGLE_EDITS",
]

genrule(
    name = "generated_headers",
    outs = [
        "src/inet/features.h",
        "src/inet/opp_defines.h",
    ],
    cmd = "touch $(OUTS)",
)

filegroup(
    name = "inet_src",
    data = glob(["src/**/*"]),
)

cc_library(
    name = "generated_headers_lib",
    hdrs = [":generated_headers"],
)

omnetpp_msg(
    name = "messages",
    srcs = glob(
        ["src/**/*.msg"],
        exclude = EXCLUDED_SOURCE_DIRS,
    ),
    import_paths = ["src"],
)

cc_library(
    name = "omnetpp_inet",
    srcs = glob(
        ["src/**/*.cc"],
        exclude = EXCLUDED_SOURCE_DIRS,
    ) + [
        ":generated_headers",
        ":messages",
    ],
    copts = COPTS,
    includes = ["src"],
    textual_hdrs = glob(
        ["src/**/*.h"],
        exclude = EXCLUDED_SOURCE_DIRS,
    ) + [
        ":messages",
    ],
    deps = [
        "@omnetpp",
    ],
    alwayslink = 1,
)
