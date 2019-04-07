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

#include <string>
#include <vector>

#include <glog/logging.h>

#include <mesos/docker/spec.hpp>

#include <mesos/secret/resolver.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/json.hpp>
#include <stout/os.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/executor.hpp>
#include <process/id.hpp>

#include <process/metrics/metrics.hpp>
#include <process/metrics/timer.hpp>

#include "slave/containerizer/mesos/provisioner/constants.hpp"
#include "slave/containerizer/mesos/provisioner/utils.hpp"

#include "slave/containerizer/mesos/provisioner/docker/metadata_manager.hpp"
#include "slave/containerizer/mesos/provisioner/docker/paths.hpp"
#include "slave/containerizer/mesos/provisioner/docker/puller.hpp"
#include "slave/containerizer/mesos/provisioner/docker/store.hpp"

#include "uri/fetcher.hpp"

namespace spec = docker::spec;

using std::list;
using std::string;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::Promise;

using process::defer;
using process::dispatch;
using process::spawn;
using process::terminate;
using process::wait;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

class StoreProcess : public Process<StoreProcess>
{
public:
  StoreProcess(
      const Flags& _flags,
      const Owned<MetadataManager>& _metadataManager,
      const Owned<Puller>& _puller)
    : ProcessBase(process::ID::generate("docker-provisioner-store")),
      flags(_flags),
      metadataManager(_metadataManager),
      puller(_puller)
  {
  }

  ~StoreProcess() override {}

  Future<Nothing> recover();

  Future<ImageInfo> get(
      const mesos::Image& image,
      const string& backend);

  Future<Nothing> prune(
      const std::vector<mesos::Image>& excludeImages,
      const hashset<string>& activeLayerPaths);

private:
  struct Metrics
  {
    Metrics() :
        image_pull(
          "containerizer/mesos/provisioner/docker_store/image_pull", Hours(1))
    {
      process::metrics::add(image_pull);
    }

    ~Metrics()
    {
      process::metrics::remove(image_pull);
    }

    process::metrics::Timer<Milliseconds> image_pull;
  };

  Future<Image> _get(
      const spec::ImageReference& reference,
      const Option<Secret>& config,
      const Option<Image>& image,
      const string& backend);

  Future<ImageInfo> __get(
      const Image& image,
      const string& backend);

  Future<vector<string>> moveLayers(
      const string& staging,
      const vector<string>& layerIds,
      const string& backend);

  Future<Nothing> moveLayer(
      const string& staging,
      const string& layerId,
      const string& backend);

  Future<Nothing> _prune(
      const hashset<string>& activeLayerPaths,
      const hashset<string>& retainedImageLayers);

  const Flags flags;

  Owned<MetadataManager> metadataManager;
  Owned<Puller> puller;
  hashmap<string, Owned<Promise<Image>>> pulling;

  // For executing path removals in a separated actor.
  process::Executor executor;

  Metrics metrics;
};


Try<Owned<slave::Store>> Store::create(
    const Flags& flags,
    SecretResolver* secretResolver)
{
  // TODO(jieyu): We should inject URI fetcher from top level, instead
  // of creating it here.
  uri::fetcher::Flags _flags;

  // TODO(dpravat): Remove after resolving MESOS-5473.
#ifndef __WINDOWS__
  _flags.docker_config = flags.docker_config;
  _flags.docker_stall_timeout = flags.fetcher_stall_timeout;
#endif

  if (flags.hadoop_home.isSome()) {
    _flags.hadoop_client = path::join(flags.hadoop_home.get(), "bin", "hadoop");
  }

  Try<Owned<uri::Fetcher>> fetcher = uri::fetcher::create(_flags);
  if (fetcher.isError()) {
    return Error("Failed to create the URI fetcher: " + fetcher.error());
  }

  Try<Owned<Puller>> puller =
    Puller::create(flags, fetcher->share(), secretResolver);

  if (puller.isError()) {
    return Error("Failed to create Docker puller: " + puller.error());
  }

  Try<Owned<slave::Store>> store = Store::create(flags, puller.get());
  if (store.isError()) {
    return Error("Failed to create Docker store: " + store.error());
  }

  return store.get();
}


Try<Owned<slave::Store>> Store::create(
    const Flags& flags,
    const Owned<Puller>& puller)
{
  Try<Nothing> mkdir = os::mkdir(flags.docker_store_dir);
  if (mkdir.isError()) {
    return Error("Failed to create Docker store directory: " +
                 mkdir.error());
  }

  mkdir = os::mkdir(paths::getStagingDir(flags.docker_store_dir));
  if (mkdir.isError()) {
    return Error("Failed to create Docker store staging directory: " +
                 mkdir.error());
  }

  mkdir = os::mkdir(paths::getGcDir(flags.docker_store_dir));
  if (mkdir.isError()) {
    return Error("Failed to create Docker store gc directory: " +
                 mkdir.error());
  }

  Try<Owned<MetadataManager>> metadataManager = MetadataManager::create(flags);
  if (metadataManager.isError()) {
    return Error(metadataManager.error());
  }

  Owned<StoreProcess> process(
      new StoreProcess(flags, metadataManager.get(), puller));

  return Owned<slave::Store>(new Store(process));
}


Store::Store(Owned<StoreProcess> _process) : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


Store::~Store()
{
  terminate(process.get());
  wait(process.get());
}


Future<Nothing> Store::recover()
{
  return dispatch(process.get(), &StoreProcess::recover);
}


Future<ImageInfo> Store::get(
    const mesos::Image& image,
    const string& backend)
{
  return dispatch(process.get(), &StoreProcess::get, image, backend);
}


Future<Nothing> Store::prune(
    const vector<mesos::Image>& excludedImages,
    const hashset<string>& activeLayerPaths)
{
  return dispatch(
      process.get(), &StoreProcess::prune, excludedImages, activeLayerPaths);
}


Future<Nothing> StoreProcess::recover()
{
  return metadataManager->recover();
}


Future<ImageInfo> StoreProcess::get(
    const mesos::Image& image,
    const string& backend)
{
  if (image.type() != mesos::Image::DOCKER) {
    return Failure("Docker provisioner store only supports Docker images");
  }

  Try<spec::ImageReference> reference =
    spec::parseImageReference(image.docker().name());

  if (reference.isError()) {
    return Failure("Failed to parse docker image '" + image.docker().name() +
                   "': " + reference.error());
  }

  return metadataManager->get(reference.get(), image.cached())
    .then(defer(self(),
                &Self::_get,
                reference.get(),
                image.docker().has_config()
                  ? image.docker().config()
                  : Option<Secret>(),
                lambda::_1,
                backend))
    .then(defer(self(), &Self::__get, lambda::_1, backend));
}


Future<Image> StoreProcess::_get(
    const spec::ImageReference& reference,
    const Option<Secret>& config,
    const Option<Image>& image,
    const string& backend)
{
  // NOTE: Here, we assume that image layers are not removed without
  // first removing the metadata in the metadata manager first.
  // Otherwise, the image we return here might miss some layers. At
  // the time we introduce cache eviction, we also want to avoid the
  // situation where a layer was returned to the provisioner but is
  // later evicted.
  if (image.isSome()) {
    // It is possible that a layer is missed after recovery if the
    // agent flag `--image_provisioner_backend` is changed from a
    // specified backend to `None()`. We need to check that each
    // layer exists for a cached image.
    bool layerMissed = false;

    foreach (const string& layerId, image->layer_ids()) {
      const string rootfsPath = paths::getImageLayerRootfsPath(
          flags.docker_store_dir,
          layerId,
          backend);

      if (!os::exists(rootfsPath)) {
        layerMissed = true;
        break;
      }
    }

    if (!layerMissed) {
      return image.get();
    }
  }

  // If there is already a pulling going on for the given 'name', we
  // will skip the additional pulling.
  const string name = stringify(reference);

  if (!pulling.contains(name)) {
    Try<string> staging =
      os::mkdtemp(paths::getStagingTempDir(flags.docker_store_dir));

    if (staging.isError()) {
      return Failure(
          "Failed to create a staging directory: " + staging.error());
    }

    Owned<Promise<Image>> promise(new Promise<Image>());

    Future<Image> future = metrics.image_pull.time(puller->pull(
        reference,
        staging.get(),
        backend,
        config)
      .then(defer(self(),
                  &Self::moveLayers,
                  staging.get(),
                  lambda::_1,
                  backend))
      .then(defer(self(), [=](const vector<string>& layerIds) {
        return metadataManager->put(reference, layerIds);
      }))
      .onAny(defer(self(), [=](const Future<Image>&) {
        pulling.erase(name);

        Try<Nothing> rmdir = os::rmdir(staging.get());
        if (rmdir.isError()) {
          LOG(WARNING) << "Failed to remove staging directory: "
                       << rmdir.error();
        }
      })));

    promise->associate(future);
    pulling[name] = promise;

    return promise->future();
  }

  return pulling[name]->future();
}


Future<ImageInfo> StoreProcess::__get(
    const Image& image,
    const string& backend)
{
  CHECK_LT(0, image.layer_ids_size());

  vector<string> layerPaths;
  foreach (const string& layerId, image.layer_ids()) {
    layerPaths.push_back(paths::getImageLayerRootfsPath(
        flags.docker_store_dir,
        layerId,
        backend));
  }

  const string path = paths::getImageLayerManifestPath(
      flags.docker_store_dir,
      image.layer_ids(image.layer_ids_size() - 1));

  // Read the manifest from the last layer because all runtime config
  // are merged at the leaf already.
  Try<string> manifest = os::read(path);
  if (manifest.isError()) {
    return Failure(
        "Failed to read manifest from '" + path + "': " +
        manifest.error());
  }

  Try<::docker::spec::v1::ImageManifest> v1 =
    ::docker::spec::v1::parse(manifest.get());

  if (v1.isError()) {
    return Failure(
        "Failed to parse docker v1 manifest from '" + path + "': " +
        v1.error());
  }

  return ImageInfo{layerPaths, v1.get()};
}


Future<vector<string>> StoreProcess::moveLayers(
    const string& staging,
    const vector<string>& layerIds,
    const string& backend)
{
  vector<Future<Nothing>> futures;
  foreach (const string& layerId, layerIds) {
    futures.push_back(moveLayer(staging, layerId, backend));
  }

  return collect(futures)
    .then([layerIds]() -> vector<string> { return layerIds; });
}


Future<Nothing> StoreProcess::moveLayer(
    const string& staging,
    const string& layerId,
    const string& backend)
{
  const string source = path::join(staging, layerId);

  // This is the case where the puller skips the pulling of the layer
  // because the layer already exists in the store.
  //
  // TODO(jieyu): Verify that the layer is actually in the store.
  if (!os::exists(source)) {
    return Nothing();
  }

  const string targetRootfs = paths::getImageLayerRootfsPath(
      flags.docker_store_dir,
      layerId,
      backend);

  // NOTE: Since the layer id is supposed to be unique. If the layer
  // already exists in the store, we'll skip the moving since they are
  // expected to be the same.
  if (os::exists(targetRootfs)) {
    return Nothing();
  }

  const string sourceRootfs = paths::getImageLayerRootfsPath(source, backend);
  const string target = paths::getImageLayerPath(
      flags.docker_store_dir,
      layerId);

#ifdef __linux__
  // If the backend is "overlay", we need to convert
  // AUFS whiteout files to OverlayFS whiteout files.
  if (backend == OVERLAY_BACKEND) {
    Try<Nothing> convert = convertWhiteouts(sourceRootfs);
    if (convert.isError()) {
      return Failure(
          "Failed to convert the whiteout files under '" +
          sourceRootfs + "': " + convert.error());
    }
  }
#endif

  if (!os::exists(target)) {
    // This is the case that we pull the layer for the first time.
    Try<Nothing> mkdir = os::mkdir(target);
    if (mkdir.isError()) {
      return Failure(
          "Failed to create directory in store for layer '" +
          layerId + "': " + mkdir.error());
    }

    Try<Nothing> rename = os::rename(source, target);
    if (rename.isError()) {
      return Failure(
          "Failed to move layer from '" + source +
          "' to '" + target + "': " + rename.error());
    }
  } else {
    // This is the case where the layer has already been pulled with a
    // different backend.
    Try<Nothing> rename = os::rename(sourceRootfs, targetRootfs);
    if (rename.isError()) {
      return Failure(
          "Failed to move rootfs from '" + sourceRootfs +
          "' to '" + targetRootfs + "': " + rename.error());
    }
  }

  return Nothing();
}


Future<Nothing> StoreProcess::prune(
    const vector<mesos::Image>& excludedImages,
    const hashset<string>& activeLayerPaths)
{
  // All existing pulling should have finished.
  if (!pulling.empty()) {
    return Failure("Cannot prune and pull at the same time");
  }

  vector<spec::ImageReference> imageReferences;
  imageReferences.reserve(excludedImages.size());

  foreach (const mesos::Image& image, excludedImages) {
    Try<spec::ImageReference> reference =
      spec::parseImageReference(image.docker().name());

    if (reference.isError()) {
      return Failure(
          "Failed to parse docker image '" + image.docker().name() +
          "': " + reference.error());
    }

    imageReferences.push_back(reference.get());
  }

  return metadataManager->prune(imageReferences)
      .then(defer(self(), &Self::_prune, activeLayerPaths, lambda::_1));
}


Future<Nothing> StoreProcess::_prune(
    const hashset<string>& activeLayerRootfses,
    const hashset<string>& retainedLayerIds)
{
  Try<list<string>> allLayers = paths::listLayers(flags.docker_store_dir);
  if (allLayers.isError()) {
    return Failure("Failed to find all layer paths: " + allLayers.error());
  }

  // Paths in provisioner are layer rootfs. Normalize them to layer
  // path.
  hashset<string> activeLayerPaths;

  foreach (const string& rootfsPath, activeLayerRootfses) {
    activeLayerPaths.insert(Path(rootfsPath).dirname());
  }

  foreach (const string& layerId, allLayers.get()) {
    if (retainedLayerIds.contains(layerId)) {
      VLOG(1) << "Layer '" << layerId << "' is retained by image store cache";
      continue;
    }

    const string layerPath =
      paths::getImageLayerPath(flags.docker_store_dir, layerId);

    if (activeLayerPaths.contains(layerPath)) {
      VLOG(1) << "Layer '" << layerId << "' is retained by active container";
      continue;
    }

    const string target =
      paths::getGcLayerPath(flags.docker_store_dir, layerId);

    if (os::exists(target)) {
      return Failure("Marking phase target '" + target + "' already exists");
    }

    VLOG(1) << "Marking layer '" << layerId << "' to gc by renaming '"
            << layerPath << "' to '" << target << "'";

    Try<Nothing> rename = os::rename(layerPath, target);
    if (rename.isError()) {
      return Failure(
          "Failed to move layer from '" + layerPath +
          "' to '" + target + "': " + rename.error());
    }
  }

  const string gcDir = paths::getGcDir(flags.docker_store_dir);
  auto rmdirs = [gcDir]() {
    Try<list<string>> targets = os::ls(gcDir);
    if (targets.isError()) {
      LOG(WARNING) << "Error when listing gcDir '" << gcDir
                   << "': " << targets.error();
      return Nothing();
    }

    foreach (const string& target, targets.get()) {
      const string path = path::join(gcDir, target);
      // Run the removal operation with 'continueOnError = false'.
      // A possible situation is that we incorrectly marked a layer
      // which is still used by certain layer based backends (aufs, overlay).
      // In such a case, we proceed with a warning and try to free up as much
      // disk spaces as possible.
      LOG(INFO) << "Deleting path '" << path << "'";
      Try<Nothing> rmdir = os::rmdir(path, true, true, false);

      if (rmdir.isError()) {
        LOG(WARNING) << "Failed to delete '" << path << "': "
                     << rmdir.error();
      } else {
        LOG(INFO) << "Deleted '" << path << "'";
      }
    }

    return Nothing();
  };

  // NOTE: All `rmdirs` calls are dispatched to one executor so that:
  //   1. They do not block other dispatches;
  //   2. They do not occupy all worker threads.
  executor.execute(rmdirs);

  return Nothing();
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
