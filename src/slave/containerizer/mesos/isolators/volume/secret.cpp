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

#include "slave/containerizer/mesos/isolators/volume/secret.hpp"

#include <string>
#include <vector>

#include <mesos/secret/resolver.hpp>

#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>

#include <stout/foreach.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include <stout/os/mkdir.hpp>
#include <stout/os/touch.hpp>
#include <stout/os/write.hpp>

#ifdef __linux__
#include "linux/ns.hpp"
#endif // __linux__

#include "common/validation.hpp"

using std::string;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

constexpr char SECRET_DIR[] = ".secret";


Try<Isolator*> VolumeSecretIsolatorProcess::create(
    const Flags& flags,
    SecretResolver* secretResolver)
{
  if (flags.launcher != "linux" ||
      !strings::contains(flags.isolation, "filesystem/linux")) {
    return Error("Volume secret isolation requires filesystem/linux isolator.");
  }

  const string hostSecretTmpDir = path::join(flags.runtime_dir, SECRET_DIR);

  Try<Nothing> mkdir = os::mkdir(hostSecretTmpDir);
  if (mkdir.isError()) {
    return Error("Failed to create secret directory on the host tmpfs:" +
                 mkdir.error());
  }

  Owned<MesosIsolatorProcess> process(new VolumeSecretIsolatorProcess(
      flags,
      secretResolver));

  return new MesosIsolator(process);
}


VolumeSecretIsolatorProcess::VolumeSecretIsolatorProcess(
    const Flags& _flags,
    SecretResolver* secretResolver)
  : ProcessBase(process::ID::generate("volume-secret-isolator")),
    flags(_flags),
    secretResolver(secretResolver) {}


bool VolumeSecretIsolatorProcess::supportsNesting()
{
  return true;
}


Future<Option<ContainerLaunchInfo>> VolumeSecretIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  if (!containerConfig.has_container_info()) {
    return None();
  }

  const ContainerInfo& containerInfo = containerConfig.container_info();

  if (containerInfo.type() != ContainerInfo::MESOS) {
    return Failure(
        "Can only prepare the secret volume isolator for a MESOS container");
  }

  if (containerConfig.has_container_class() &&
      containerConfig.container_class() == ContainerClass::DEBUG) {
    return None();
  }

  ContainerLaunchInfo launchInfo;
  launchInfo.add_clone_namespaces(CLONE_NEWNS);

  const string sandboxSecretRootDir =
    path::join(containerConfig.directory(),
               SECRET_DIR + string("-") + stringify(id::UUID::random()));

  // TODO(Kapil): Add some UUID suffix to the secret-root dir to avoid conflicts
  // with user container_path.
  Try<Nothing> mkdir = os::mkdir(sandboxSecretRootDir);
  if (mkdir.isError()) {
    return Failure("Failed to create sandbox secret root directory at '" +
                   sandboxSecretRootDir + "': " + mkdir.error());
  }

  // Mount ramfs in the container.
  CommandInfo* command = launchInfo.add_pre_exec_commands();
  command->set_shell(false);
  command->set_value("mount");
  command->add_arguments("mount");
  command->add_arguments("-n");
  command->add_arguments("-t");
  command->add_arguments("ramfs");
  command->add_arguments("ramfs");
  command->add_arguments(sandboxSecretRootDir);

  vector<Future<Nothing>> futures;
  foreach (const Volume& volume, containerInfo.volumes()) {
    if (!volume.has_source() ||
        !volume.source().has_type() ||
        volume.source().type() != Volume::Source::SECRET) {
      continue;
    }

    if (!volume.source().has_secret()) {
      return Failure("volume.source.secret is not specified");
    }

    if (secretResolver == nullptr) {
      return Failure(
          "Error: Volume has secret but no secret-resolver provided");
    }

    const Secret& secret = volume.source().secret();

    Option<Error> error = common::validation::validateSecret(secret);
    if (error.isSome()) {
      return Failure("Invalid secret specified in volume: " + error->message);
    }

    string targetContainerPath;
    if (path::absolute(volume.container_path())) {
      if (containerConfig.has_rootfs()) {
        targetContainerPath = path::join(
            containerConfig.rootfs(),
            volume.container_path());

        Try<Nothing> mkdir = os::mkdir(Path(targetContainerPath).dirname());
        if (mkdir.isError()) {
          return Failure(
              "Failed to create directory '" +
              Path(targetContainerPath).dirname() + "' "
              "for the target mount file: " + mkdir.error());
        }

        Try<Nothing> touch = os::touch(targetContainerPath);
        if (touch.isError()) {
          return Failure(
              "Failed to create the target mount file at '" +
              targetContainerPath + "': " + touch.error());
        }
      } else {
        targetContainerPath = volume.container_path();

        if (!os::exists(targetContainerPath)) {
          return Failure(
              "Absolute container path '" + targetContainerPath + "' "
              "does not exist");
        }
      }
    } else {
      if (containerConfig.has_rootfs()) {
        targetContainerPath = path::join(
            containerConfig.rootfs(),
            flags.sandbox_directory,
            volume.container_path());
      } else {
        targetContainerPath = path::join(
            containerConfig.directory(),
            volume.container_path());
      }

      // Create the mount point if bind mount is used.
      // NOTE: We cannot create the mount point at 'targetContainerPath' if
      // container has rootfs defined. The bind mount of the sandbox
      // will hide what's inside 'targetContainerPath'. So we should always
      // create the mount point in the sandbox.
      const string mountPoint = path::join(
          containerConfig.directory(),
          volume.container_path());

      Try<Nothing> mkdir = os::mkdir(Path(mountPoint).dirname());
      if (mkdir.isError()) {
        return Failure(
            "Failed to create the target mount file directory at '" +
            Path(mountPoint).dirname() + "': " + mkdir.error());
      }

      Try<Nothing> touch = os::touch(mountPoint);
      if (touch.isError()) {
        return Failure(
            "Failed to create the target mount file at '" +
            targetContainerPath + "': " + touch.error());
      }
    }

    const string hostSecretPath =
      path::join(flags.runtime_dir, SECRET_DIR, stringify(id::UUID::random()));

    const string sandboxSecretPath =
      path::join(sandboxSecretRootDir,
                 volume.container_path());

    Try<Nothing> mkdir = os::mkdir(Path(sandboxSecretPath).dirname());
    if (mkdir.isError()) {
      return Failure(
          "Failed to create the target mount file directory at '" +
          Path(sandboxSecretPath).dirname() + "': " + mkdir.error());
    }

    // Create directory tree inside sandbox secret root dir.
    command = launchInfo.add_pre_exec_commands();
    command->set_shell(false);
    command->set_value("mkdir");
    command->add_arguments("mkdir");
    command->add_arguments("-p");
    command->add_arguments(Path(sandboxSecretPath).dirname());

    // Move secret from hostSecretPath to sandboxSecretPath.
    command = launchInfo.add_pre_exec_commands();
    command->set_shell(false);
    command->set_value("mv");
    command->add_arguments("mv");
    command->add_arguments("-f");
    command->add_arguments(hostSecretPath);
    command->add_arguments(sandboxSecretPath);

    // Bind mount sandboxSecretPath to targetContainerPath
    command = launchInfo.add_pre_exec_commands();
    command->set_shell(false);
    command->set_value("mount");
    command->add_arguments("mount");
    command->add_arguments("-n");
    command->add_arguments("--rbind");
    command->add_arguments(sandboxSecretPath);
    command->add_arguments(targetContainerPath);

    // If the mount needs to be read-only, do a remount.
    if (volume.mode() == Volume::RO) {
      command = launchInfo.add_pre_exec_commands();
      command->set_shell(false);
      command->set_value("mount");
      command->add_arguments("mount");
      command->add_arguments("-n");
      command->add_arguments("-o");
      command->add_arguments("bind,ro,remount");
      command->add_arguments(sandboxSecretPath);
      command->add_arguments(targetContainerPath);
    }

    Future<Nothing> future = secretResolver->resolve(secret)
      .then([hostSecretPath](const Secret::Value& value) -> Future<Nothing> {
        Try<Nothing> writeSecret = os::write(hostSecretPath, value.data());
        if (writeSecret.isError()) {
          return Failure(
              "Error writing secret to '" + hostSecretPath + "': " +
              writeSecret.error());
        }
        return Nothing();
      });

    futures.push_back(future);
  }

  return collect(futures)
    .then([launchInfo, containerId](
        const vector<Nothing>& results) -> Future<Option<ContainerLaunchInfo>> {
      LOG(INFO) << results.size() << " secrets have been resolved for "
                << "container " << containerId;
      return launchInfo;
    });
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
