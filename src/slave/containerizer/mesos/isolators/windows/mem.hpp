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

#ifndef __WINDOWS_MEM_ISOLATOR_HPP__
#define __WINDOWS_MEM_ISOLATOR_HPP__

#include <vector>

#include <process/future.hpp>
#include <process/id.hpp>

#include <stout/bytes.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

namespace mesos {
namespace internal {
namespace slave {

class WindowsMemIsolatorProcess final : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags);

  bool supportsNesting() override;
  bool supportsStandalone() override;

  process::Future<Nothing> recover(
      const std::vector<mesos::slave::ContainerState>& state,
      const hashset<ContainerID>& orphans) override;

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

  process::Future<Nothing> isolate(
      const ContainerID& containerId, pid_t pid) override;

  process::Future<Nothing> update(
      const ContainerID& containerId, const Resources& resources) override;

  process::Future<ResourceStatistics> usage(
      const ContainerID& containerId) override;

  process::Future<Nothing> cleanup(const ContainerID& containerId) override;

private:
  struct Info
  {
    Option<pid_t> pid;
    Option<Bytes> limit;
  };

  hashmap<ContainerID, Info> infos;

  WindowsMemIsolatorProcess()
    : ProcessBase(process::ID::generate("windows-mem-isolator"))
  {}
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __WINDOWS_MEM_ISOLATOR_HPP__
