#!/bin/bash
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


set -e

readarray -td, srcs <<< $1;
readarray -td, import_paths <<< $3;
output_path=$2;
opp_msgc_path=$4;

function main() {
  # Copy all sources to output directory, since opp_msgc always puts its output
  # in the same directory as the source file (and this is not configurable).
  for src in ${srcs[@]}; do
    mkdir -p $(dirname "${output_path}/${src}")
    cp "${src}" "${output_path}/${src}"
  done

  # Set up compiler flags.
  flags="--msg6"
  for import_path in ${import_paths[@]}; do
    flags+=" -I${import_path}"
  done

  # Invoke compiler for each source file.
  for src in ${srcs[@]}; do
    "${opp_msgc_path}" ${flags} "${output_path}/${src}"
  done
}

main
