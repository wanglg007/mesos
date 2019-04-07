#!/usr/bin/env bash

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# This is a script for building Mesos website.
set -e
set -o pipefail

function exit_hook {
  # Remove generated documents when exit.
  cd /mesos/site && bundle exec rake clean_docs
}

trap exit_hook EXIT

# Build mesos to get the latest master and agent binaries.
./bootstrap
mkdir -p build
pushd build
../configure --disable-python
make -j6
popd # build

# Generate the endpoint docs from the latest mesos and agent binaries.
./support/generate-endpoint-help.py

# Build the website.
pushd site
bundle exec rake
popd # site
