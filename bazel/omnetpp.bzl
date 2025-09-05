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

"""Build rules for OMNeT++."""

def omnetpp_msg(name, srcs, import_paths = [], **kwargs):
    header_names = [src.replace(".msg", "_m.h") for src in srcs]
    source_names = [src.replace(".msg", "_m.cc") for src in srcs]

    native.genrule(
        name = name,
        srcs = srcs,
        outs = header_names + source_names,
        cmd = """
          $(location @omnetpp//:opp_msgc_wrapper) "$(SRCS)" "$(GENDIR)" {0} "$(location @omnetpp//:opp_msgc)"
        """.format('""' + ",".join(["$(RULEDIR)/" + path for path in import_paths])),
        tools = [
            "@omnetpp//:opp_msgc",
            "@omnetpp//:opp_msgc_wrapper",
        ],
    )
