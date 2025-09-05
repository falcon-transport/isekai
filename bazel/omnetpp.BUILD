load("@//bazel:bison.bzl", "genyacc")
load("@//bazel:flex.bzl", "genlex")
load("@//bazel:omnetpp.bzl", "omnetpp_msg")

package(default_visibility = ["//visibility:public"])

COPTS = [
    "-fexceptions",
    "-DOMNETPP_IMAGE_PATH=\\\"\\\"",
    "-DWITH_NETBUILDER",
]

FEATURES = [
    "-layering_check",
    "-use_header_modules",
]

cc_library(
    name = "omnetpp_headers",
    includes = ["include"],
    textual_hdrs = glob(["include/**/*.h"]),
)

# See ./configure
genrule(
    name = "version_file",
    srcs = [
        "src/common/ver.h.base",
        "Version",
    ],
    outs = ["src/common/ver.h"],
    cmd = """
      OMNETPP_PRODUCT="OMNeT++"
      OMNETPP_RELEASE=`cat $(location Version)`
      OMNETPP_VERSION=`cat $(location Version) | sed 's/^.*-//'`
      OMNETPP_BUILDID="200207-b15dab9895"
      OMNETPP_EDITION="Academic Public License -- NOT FOR COMMERCIAL USE"
      cat $(location src/common/ver.h.base) >$@ &&
      echo "#define OMNETPP_PRODUCT \\"$$OMNETPP_PRODUCT\\"" >>$@ &&
      echo "#define OMNETPP_RELEASE \\"$$OMNETPP_RELEASE\\"" >>$@ &&
      echo "#define OMNETPP_VERSION_STR \\"$$OMNETPP_VERSION\\"" >>$@ &&
      echo "#define OMNETPP_BUILDID \\"$$OMNETPP_BUILDID\\"" >>$@ &&
      echo "#define OMNETPP_EDITION \\"$$OMNETPP_EDITION\\"" >>$@
    """,
)

genlex(
    name = "expression_lexer",
    src = "src/common/expression.lex",
    out = "src/common/lex.expressionyy.cc",
    prefix = "expressionyy",
)

genyacc(
    name = "expression_parser",
    src = "src/common/expression.y",
    header_out = "src/common/expression.tab.hh",
    prefix = "expressionyy",
    source_out = "src/common/expression.tab.cc",
)

genyacc(
    name = "matchexpression_parser",
    src = "src/common/matchexpression.y",
    header_out = "src/common/matchexpression.tab.hh",
    prefix = "matchexpressionyy",
    source_out = "src/common/matchexpression.tab.cc",
)

cc_library(
    name = "oppcommon",
    srcs = glob([
        "src/common/*.c",
        "src/common/*.cc",
    ]) + [
        ":version_file",
        ":expression_lexer",
        ":expression_parser",
        ":matchexpression_parser",
    ],
    copts = COPTS,
    features = FEATURES,
    includes = [
        "include",
        "src",
        "src/common",
    ],
    textual_hdrs = glob(["src/common/*.h"]),
    deps = [":omnetpp_headers"],
)

cc_library(
    name = "oppcmdenv",
    srcs = glob(["src/cmdenv/*.cc"]),
    copts = COPTS,
    features = FEATURES,
    textual_hdrs = glob(["src/cmdenv/*.h"]),
    deps = [
        ":omnetpp_headers",
        ":oppcommon",
        ":oppenvir",
    ],
    alwayslink = 1,
)

genrule(
    name = "envir_generated_files",
    srcs = [
        "src/envir/eventlogwriter.pl",
        "src/eventlog/eventlogentries.txt",
    ],
    outs = [
        "src/envir/eventlogwriter.h",
        "src/envir/eventlogwriter.cc",
    ],
    cmd = """
      cp $(location src/envir/eventlogwriter.pl) $(RULEDIR)/src/envir/eventlogwriter.pl &&
      mkdir -p $(RULEDIR)/src/eventlog
      cp $(location src/eventlog/eventlogentries.txt) $(RULEDIR)/src/eventlog/eventlogentries.txt &&
      cd $(RULEDIR)/src/envir &&
      perl eventlogwriter.pl
    """,
)

cc_library(
    name = "oppenvir",
    srcs = glob(
        ["src/envir/*.cc"],
        exclude = ["src/envir/main.cc"],
    ) + [
        ":envir_generated_files",
    ],
    copts = COPTS,
    features = FEATURES,
    includes = [
        "include",
        "src",
        "src/envir",
    ],
    textual_hdrs = glob(["src/envir/*.h"]),
    deps = [
        ":omnetpp_headers",
        ":oppcommon",
        ":oppnedxml",
        ":oppsim",
    ],
)

cc_library(
    name = "oppmain",
    srcs = ["src/envir/main.cc"],
    copts = COPTS,
    features = FEATURES,
    includes = [
        "include",
        "src",
        "src/envir",
    ],
    deps = [":oppenvir"],
)

genrule(
    name = "msg_generated_files",
    srcs = [
        "src/nedxml/dtdclassgen.pl",
        "doc/etc/msg2.dtd",
    ],
    outs = [
        "src/nedxml/msgelements.h",
        "src/nedxml/msgelements.cc",
        "src/nedxml/msgdtdvalidator.h",
        "src/nedxml/msgdtdvalidator.cc",
        "src/nedxml/msgvalidator.h",
        "src/nedxml/msgvalidator.cc",
    ],
    cmd = """
      cp $(location src/nedxml/dtdclassgen.pl) $(RULEDIR)/src/nedxml/dtdclassgen.pl &&
      cp $(location doc/etc/msg2.dtd) $(RULEDIR)/src/nedxml/msg2.dtd &&
      cd $(RULEDIR)/src/nedxml &&
      perl dtdclassgen.pl msg2.dtd msg
    """,
)

genlex(
    name = "msg_lexer",
    src = "src/nedxml/msg2.lex",
    out = "src/nedxml/lex.msg2.cc",
    prefix = "msg2yy",
)

genyacc(
    name = "msg_parser",
    src = "src/nedxml/msg2.y",
    header_out = "src/nedxml/msg2.tab.hh",
    prefix = "msg2yy",
    source_out = "src/nedxml/msg2.tab.cc",
)

genrule(
    name = "ned_generated_files",
    srcs = [
        "src/nedxml/dtdclassgen.pl",
        "doc/etc/ned2.dtd",
    ],
    outs = [
        "src/nedxml/nedelements.h",
        "src/nedxml/nedelements.cc",
        "src/nedxml/neddtdvalidator.h",
        "src/nedxml/neddtdvalidator.cc",
        "src/nedxml/nedvalidator.h",
        "src/nedxml/nedvalidator.cc",
    ],
    cmd = """
      cp $(location src/nedxml/dtdclassgen.pl) $(RULEDIR)/src/nedxml/dtdclassgen.pl &&
      cp $(location doc/etc/ned2.dtd) $(RULEDIR)/src/nedxml/ned2.dtd &&
      cd $(RULEDIR)/src/nedxml &&
      perl dtdclassgen.pl ned2.dtd ned
    """,
)

genlex(
    name = "ned_lexer",
    src = "src/nedxml/ned2.lex",
    out = "src/nedxml/lex.ned2.cc",
    prefix = "ned2yy",
)

genyacc(
    name = "ned_parser",
    src = "src/nedxml/ned2.y",
    header_out = "src/nedxml/ned2.tab.hh",
    prefix = "ned2yy",
    source_out = "src/nedxml/ned2.tab.cc",
)

# See https://github.com/omnetpp/omnetpp/blob/f0a213dad1597c6ff9934b6320da828c99531762/src/nedxml/Makefile#L150
genrule(
    name = "sim_std_msg",
    srcs = ["src/sim/sim_std.msg"],
    outs = ["src/nedxml/sim_std_msg.cc"],
    cmd = """
      echo '//' >$@ &&
      echo '// THIS IS A GENERATED FILE, DO NOT EDIT!' >>$@ &&
      echo '//' >>$@ &&
      echo '' >>$@ &&
      echo 'namespace omnetpp { namespace nedxml { const char *SIM_STD_DEFINITIONS = R"ENDMARK(' >>$@ &&
      cat $< >>$@ &&
      echo ')ENDMARK"; } }' >>$@
    """,
)

cc_library(
    name = "oppnedxml",
    srcs = glob(
        ["src/nedxml/*.cc"],
        exclude = [
            # Disable alternate XML parser implementations
            "src/nedxml/saxparser_expat.cc",
            "src/nedxml/saxparser_none.cc",
            # Don't compile command-line tools as part of the library
            "src/nedxml/opp_msgtool.cc",
            "src/nedxml/opp_nedtool.cc",
        ],
    ) + [
        ":msg_generated_files",
        ":msg_lexer",
        ":ned_generated_files",
        ":ned_lexer",
        ":sim_std_msg",
        # These files are included by name because the corresponding headers
        # must be in textual_hdrs (not srcs) to prevent compilation errors.
        "src/nedxml/msg2.tab.cc",
        "src/nedxml/ned2.tab.cc",
    ],
    copts = COPTS,
    features = FEATURES,
    includes = [
        "include",
        "src",
        "src/nedxml",
    ],
    textual_hdrs = glob(["src/nedxml/*.h"]) + [
        "src/nedxml/msg2.tab.hh",
        "src/nedxml/ned2.tab.hh",
    ],
    deps = [
        ":omnetpp_headers",
        ":oppcommon",
        "@//bazel:libxml2",
    ],
)

genlex(
    name = "expr_lexer",
    src = "src/sim/expr.lex",
    out = "src/sim/lex.expryy.cc",
    prefix = "expryy",
)

genyacc(
    name = "expr_parser",
    src = "src/sim/expr.y",
    header_out = "src/sim/expr.tab.hh",
    prefix = "expryy",
    source_out = "src/sim/expr.tab.cc",
)

cc_library(
    name = "resultexpr",
    textual_hdrs = ["src/sim/resultexpr.h"],
    deps = [":oppcommon"],
)

omnetpp_msg(
    name = "sim_std",
    srcs = ["src/sim/sim_std.msg"],
)

cc_library(
    name = "oppsim",
    srcs = glob(
        ["src/sim/**/*.cc"],
        exclude = [
            # For some reason this isn't included in the source Makefile. It can't
            # build without errors.
            "src/sim/parsim/cadvlinkdelaylookahead.cc",
        ],
    ) + [
        ":expr_lexer",
        ":expr_parser",
        ":sim_std",
    ],
    copts = COPTS,
    features = FEATURES,
    includes = [
        "include",
        "src",
        "src/sim",
    ],
    textual_hdrs = glob(["src/sim/**/*.h"]),
    deps = [
        ":omnetpp_headers",
        ":oppcommon",
        ":oppnedxml",
    ],
)

# OMNeT++ message compiler.
cc_binary(
    name = "opp_msgc",
    srcs = ["src/nedxml/opp_msgtool.cc"],
    copts = COPTS,
    features = FEATURES,
    deps = [
        ":oppnedxml",
    ],
)

# Wrapper around OMNeT++ message compiler for the omnetpp_msg build rule.
sh_binary(
    name = "opp_msgc_wrapper",
    srcs = ["@//bazel:opp_msgc_wrapper.sh"],
)

# OMNeT++ library only, without main().
cc_library(
    name = "omnetpp",
    textual_hdrs = glob(["**/*.h"]),
    deps = [
        ":oppcmdenv",
        ":oppcommon",
        ":oppenvir",
        ":oppnedxml",
        ":oppsim",
    ],
)

# OMNeT++ library, including main().
cc_library(
    name = "omnetpp_main",
    textual_hdrs = glob(["**/*.h"]),
    deps = [
        ":omnetpp",
        ":oppmain",
    ],
)
