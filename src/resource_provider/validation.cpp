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

#include "resource_provider/validation.hpp"

#include <stout/none.hpp>
#include <stout/unreachable.hpp>

using mesos::resource_provider::Call;

namespace mesos {
namespace internal {
namespace resource_provider {
namespace validation {
namespace call {

Option<Error> validate(const Call& call)
{
  if (!call.IsInitialized()) {
    return Error("Not initialized: " + call.InitializationErrorString());
  }

  if (!call.has_type()) {
    return Error("Expecting 'type' to be present");
  }

  switch(call.type()) {
    case Call::UNKNOWN: {
      return None();
    }

    case Call::SUBSCRIBE: {
      if (!call.has_subscribe()) {
        return Error("Expecting 'subscribe' to be present");
      }

      return None();
    }

    case Call::UPDATE_OPERATION_STATUS: {
      if (!call.has_resource_provider_id()) {
        return Error("Expecting 'resource_provider_id' to be present");
      }

      if (!call.has_update_operation_status()) {
        return Error("Expecting 'update_operation_status' to be present");
      }

      return None();
    }

    case Call::UPDATE_STATE: {
      if (!call.has_resource_provider_id()) {
        return Error("Expecting 'resource_provider_id' to be present");
      }

      if (!call.has_update_state()) {
        return Error("Expecting 'update_state' to be present");
      }

      return None();
    }

    case Call::UPDATE_PUBLISH_RESOURCES_STATUS: {
      if (!call.has_resource_provider_id()) {
        return Error("Expecting 'resource_provider_id' to be present");
      }

      if (!call.has_update_publish_resources_status()) {
        return Error(
            "Expecting 'update_publish_resources_status' to be present.");
      }

      return None();
    }
  }

  UNREACHABLE();
}

} // namespace call {
} // namespace validation {
} // namespace resource_provider {
} // namespace internal {
} // namespace mesos {
