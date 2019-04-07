// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __KILL_POLICY_TEST_HELPER_HPP__
#define __KILL_POLICY_TEST_HELPER_HPP__

#include <cstdint>

#include <stout/flags.hpp>
#include <stout/subcommand.hpp>

namespace mesos {
namespace internal {
namespace tests {

class KillPolicyTestHelper : public Subcommand
{
public:
  static const char NAME[];

  struct Flags : public virtual flags::FlagsBase
  {
    Flags();

    uint16_t sleep_duration;
  };

  KillPolicyTestHelper() : Subcommand(NAME) {}

  Flags flags;

protected:
  int execute() override;
  flags::FlagsBase* getFlags() override { return &flags; }
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __KILL_POLICY_TEST_HELPER_HPP__
