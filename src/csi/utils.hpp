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

#ifndef __CSI_UTILS_HPP__
#define __CSI_UTILS_HPP__

#include <ostream>
#include <type_traits>

#include <csi/spec.hpp>

#include <google/protobuf/map.h>

#include <google/protobuf/util/json_util.h>

#include <mesos/mesos.hpp>

#include <stout/foreach.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#include "csi/state.hpp"

namespace csi {
namespace v0 {

bool operator==(
    const ControllerServiceCapability& left,
    const ControllerServiceCapability& right);


bool operator==(const VolumeCapability& left, const VolumeCapability& right);


inline bool operator!=(
    const VolumeCapability& left,
    const VolumeCapability& right)
{
  return !(left == right);
}


std::ostream& operator<<(
    std::ostream& stream,
    const ControllerServiceCapability::RPC::Type& type);


// Default imprementation for output protobuf messages in namespace
// `csi`. Note that any non-template overloading of the output operator
// would take precedence over this function template.
template <
    typename Message,
    typename std::enable_if<std::is_convertible<
        Message*, google::protobuf::Message*>::value, int>::type = 0>
std::ostream& operator<<(std::ostream& stream, const Message& message)
{
  // NOTE: We use Google's JSON utility functions for proto3.
  std::string output;
  google::protobuf::util::MessageToJsonString(message, &output);
  return stream << output;
}

} // namespace v0 {
} // namespace csi {


namespace mesos {
namespace csi {
namespace v0 {

struct PluginCapabilities
{
  PluginCapabilities() = default;

  template <typename Iterable> PluginCapabilities(const Iterable& capabilities)
  {
    foreach (const auto& capability, capabilities) {
      if (capability.has_service() &&
          PluginCapability::Service::Type_IsValid(
              capability.service().type())) {
        switch (capability.service().type()) {
          case PluginCapability::Service::UNKNOWN:
            break;
          case PluginCapability::Service::CONTROLLER_SERVICE:
            controllerService = true;
            break;
          case google::protobuf::kint32min:
          case google::protobuf::kint32max:
            UNREACHABLE();
        }
      }
    }
  }

  bool controllerService = false;
};


struct ControllerCapabilities
{
  ControllerCapabilities() = default;

  template <typename Iterable>
  ControllerCapabilities(const Iterable& capabilities)
  {
    foreach (const auto& capability, capabilities) {
      if (capability.has_rpc() &&
          ControllerServiceCapability::RPC::Type_IsValid(
              capability.rpc().type())) {
        switch (capability.rpc().type()) {
          case ControllerServiceCapability::RPC::UNKNOWN:
            break;
          case ControllerServiceCapability::RPC::CREATE_DELETE_VOLUME:
            createDeleteVolume = true;
            break;
          case ControllerServiceCapability::RPC::PUBLISH_UNPUBLISH_VOLUME:
            publishUnpublishVolume = true;
            break;
          case ControllerServiceCapability::RPC::LIST_VOLUMES:
            listVolumes = true;
            break;
          case ControllerServiceCapability::RPC::GET_CAPACITY:
            getCapacity = true;
            break;
          case google::protobuf::kint32min:
          case google::protobuf::kint32max:
            UNREACHABLE();
        }
      }
    }
  }

  bool createDeleteVolume = false;
  bool publishUnpublishVolume = false;
  bool listVolumes = false;
  bool getCapacity = false;
};


struct NodeCapabilities
{
  NodeCapabilities() = default;

  template <typename Iterable> NodeCapabilities(const Iterable& capabilities)
  {
    foreach (const auto& capability, capabilities) {
      if (capability.has_rpc() &&
          NodeServiceCapability::RPC::Type_IsValid(capability.rpc().type())) {
        switch (capability.rpc().type()) {
          case NodeServiceCapability::RPC::UNKNOWN:
            break;
          case NodeServiceCapability::RPC::STAGE_UNSTAGE_VOLUME:
            stageUnstageVolume = true;
            break;
          case google::protobuf::kint32min:
          case google::protobuf::kint32max:
            UNREACHABLE();
        }
      }
    }
  }

  bool stageUnstageVolume = false;
};

} // namespace v0 {
} // namespace csi {
} // namespace mesos {

#endif // __CSI_UTILS_HPP__
