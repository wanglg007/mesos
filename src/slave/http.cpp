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

#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <mesos/attributes.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/agent/agent.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/executor/executor.hpp>

#include <mesos/slave/containerizer.hpp>

#include <mesos/v1/agent/agent.hpp>

#include <mesos/v1/executor/executor.hpp>

#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/help.hpp>
#include <process/http.hpp>
#include <process/limiter.hpp>
#include <process/logging.hpp>
#include <process/loop.hpp>
#include <process/owned.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/jsonify.hpp>
#include <stout/lambda.hpp>
#include <stout/net.hpp>
#include <stout/numify.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/unreachable.hpp>

#include "common/build.hpp"
#include "common/http.hpp"
#include "common/recordio.hpp"
#include "common/resources_utils.hpp"
#include "common/validation.hpp"

#include "internal/devolve.hpp"

#include "mesos/mesos.hpp"
#include "mesos/resources.hpp"

#include "resource_provider/local.hpp"

#include "slave/http.hpp"
#include "slave/slave.hpp"
#include "slave/validation.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/paths.hpp"

#include "version/version.hpp"

using mesos::agent::ProcessIO;

using mesos::authorization::createSubject;
using mesos::authorization::VIEW_CONTAINER;
using mesos::authorization::VIEW_FLAGS;
using mesos::authorization::VIEW_FRAMEWORK;
using mesos::authorization::VIEW_TASK;
using mesos::authorization::VIEW_EXECUTOR;
using mesos::authorization::VIEW_RESOURCE_PROVIDER;
using mesos::authorization::VIEW_ROLE;
using mesos::authorization::SET_LOG_LEVEL;
using mesos::authorization::ATTACH_CONTAINER_INPUT;
using mesos::authorization::ATTACH_CONTAINER_OUTPUT;
using mesos::authorization::LAUNCH_NESTED_CONTAINER;
using mesos::authorization::LAUNCH_NESTED_CONTAINER_SESSION;
using mesos::authorization::LAUNCH_STANDALONE_CONTAINER;
using mesos::authorization::WAIT_NESTED_CONTAINER;
using mesos::authorization::WAIT_STANDALONE_CONTAINER;
using mesos::authorization::VIEW_STANDALONE_CONTAINER;
using mesos::authorization::KILL_NESTED_CONTAINER;
using mesos::authorization::KILL_STANDALONE_CONTAINER;
using mesos::authorization::REMOVE_NESTED_CONTAINER;
using mesos::authorization::REMOVE_STANDALONE_CONTAINER;
using mesos::authorization::MODIFY_RESOURCE_PROVIDER_CONFIG;
using mesos::authorization::PRUNE_IMAGES;

using mesos::internal::recordio::Reader;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerConfig;
using mesos::slave::ContainerTermination;

using process::AUTHENTICATION;
using process::AUTHORIZATION;
using process::Break;
using process::Continue;
using process::ControlFlow;
using process::Clock;
using process::DESCRIPTION;
using process::Failure;
using process::Future;
using process::HELP;
using process::Logging;
using process::loop;
using process::Owned;
using process::TLDR;

using process::http::Accepted;
using process::http::BadRequest;
using process::http::Conflict;
using process::http::Connection;
using process::http::Forbidden;
using process::http::NotFound;
using process::http::InternalServerError;
using process::http::MethodNotAllowed;
using process::http::NotAcceptable;
using process::http::NotImplemented;
using process::http::OK;
using process::http::Pipe;
using process::http::ServiceUnavailable;
using process::http::UnsupportedMediaType;

using process::http::authentication::Principal;

using process::metrics::internal::MetricsProcess;

using ::recordio::Decoder;

using std::list;
using std::map;
using std::string;
using std::tie;
using std::tuple;
using std::vector;


namespace mesos {

static void json(JSON::ObjectWriter* writer, const TaskInfo& task)
{
  writer->field("id", task.task_id().value());
  writer->field("name", task.name());
  writer->field("slave_id", task.slave_id().value());
  writer->field("resources", task.resources());

  // Tasks are not allowed to mix resources allocated to
  // different roles, see MESOS-6636.
  writer->field("role", task.resources().begin()->allocation_info().role());

  if (task.has_command()) {
    writer->field("command", task.command());
  }
  if (task.has_executor()) {
    writer->field("executor_id", task.executor().executor_id().value());
  }
  if (task.has_discovery()) {
    writer->field("discovery", JSON::Protobuf(task.discovery()));
  }
}

static void json(
    JSON::StringWriter* writer,
    const SlaveInfo::Capability& capability)
{
  writer->set(SlaveInfo::Capability::Type_Name(capability.type()));
}

namespace internal {
namespace slave {

// Pull in the process definitions.
using process::http::Response;
using process::http::Request;


// Filtered representation of an Executor. Tasks within this executor
// are filtered based on whether the user is authorized to view them.
struct ExecutorWriter
{
  ExecutorWriter(
      const Owned<ObjectApprovers>& approvers,
      const Executor* executor,
      const Framework* framework)
    : approvers_(approvers),
      executor_(executor),
      framework_(framework) {}

  void operator()(JSON::ObjectWriter* writer) const
  {
    writer->field("id", executor_->id.value());
    writer->field("name", executor_->info.name());
    writer->field("source", executor_->info.source());
    writer->field("container", executor_->containerId.value());
    writer->field("directory", executor_->directory);
    writer->field("resources", executor_->allocatedResources());

    // Resources may be empty for command executors.
    if (!executor_->info.resources().empty()) {
      // Executors are not allowed to mix resources allocated to
      // different roles, see MESOS-6636.
      writer->field(
          "role",
          executor_->info.resources().begin()->allocation_info().role());
    }

    if (executor_->info.has_labels()) {
      writer->field("labels", executor_->info.labels());
    }

    if (executor_->info.has_type()) {
      writer->field("type", ExecutorInfo::Type_Name(executor_->info.type()));
    }

    writer->field("tasks", [this](JSON::ArrayWriter* writer) {
      foreachvalue (Task* task, executor_->launchedTasks) {
        if (!approvers_->approved<VIEW_TASK>(*task, framework_->info)) {
          continue;
        }

        writer->element(*task);
      }
    });

    writer->field("queued_tasks", [this](JSON::ArrayWriter* writer) {
      foreachvalue (const TaskInfo& task, executor_->queuedTasks) {
        if (!approvers_->approved<VIEW_TASK>(task, framework_->info)) {
          continue;
        }

        writer->element(task);
      }
    });

    writer->field("completed_tasks", [this](JSON::ArrayWriter* writer) {
      foreach (const std::shared_ptr<Task>& task, executor_->completedTasks) {
        if (!approvers_->approved<VIEW_TASK>(*task, framework_->info)) {
          continue;
        }

        writer->element(*task);
      }

      // NOTE: We add 'terminatedTasks' to 'completed_tasks' for
      // simplicity.
      foreachvalue (Task* task, executor_->terminatedTasks) {
        if (!approvers_->approved<VIEW_TASK>(*task, framework_->info)) {
          continue;
        }

        writer->element(*task);
      }
    });
  }

  const Owned<ObjectApprovers>& approvers_;
  const Executor* executor_;
  const Framework* framework_;
};

// Filtered representation of FrameworkInfo.
// Executors and Tasks are filtered based on whether the
// user is authorized to view them.
struct FrameworkWriter
{
  FrameworkWriter(
      const Owned<ObjectApprovers>& approvers,
      const Framework* framework)
    : approvers_(approvers),
      framework_(framework) {}

  void operator()(JSON::ObjectWriter* writer) const
  {
    writer->field("id", framework_->id().value());
    writer->field("name", framework_->info.name());
    writer->field("user", framework_->info.user());
    writer->field("failover_timeout", framework_->info.failover_timeout());
    writer->field("checkpoint", framework_->info.checkpoint());
    writer->field("hostname", framework_->info.hostname());

    if (framework_->info.has_principal()) {
      writer->field("principal", framework_->info.principal());
    }

    // For multi-role frameworks the `role` field will be unset.
    // Note that we could set `roles` here for both cases, which
    // would make tooling simpler (only need to look for `roles`).
    // However, we opted to just mirror the protobuf akin to how
    // generic protobuf -> JSON translation works.
    if (framework_->capabilities.multiRole) {
      writer->field("roles", framework_->info.roles());
    } else {
      writer->field("role", framework_->info.role());
    }

    writer->field("executors", [this](JSON::ArrayWriter* writer) {
      foreachvalue (Executor* executor, framework_->executors) {
        if (!approvers_->approved<VIEW_EXECUTOR>(
                executor->info, framework_->info)) {
          continue;
        }

        ExecutorWriter executorWriter(
            approvers_,
            executor,
            framework_);

        writer->element(executorWriter);
      }
    });

    writer->field(
        "completed_executors", [this](JSON::ArrayWriter* writer) {
          foreach (
              const Owned<Executor>& executor, framework_->completedExecutors) {
            if (!approvers_->approved<VIEW_EXECUTOR>(
                    executor->info, framework_->info)) {
              continue;
            }

            ExecutorWriter executorWriter(
                approvers_,
                executor.get(),
                framework_);

            writer->element(executorWriter);
          }
        });
  }

  const Owned<ObjectApprovers>& approvers_;
  const Framework* framework_;
};


string Http::API_HELP()
{
  return HELP(
    TLDR(
        "Endpoint for API calls against the agent."),
    DESCRIPTION(
        "Returns 200 OK if the call is successful"),
    AUTHENTICATION(true));
}


Future<Response> Http::api(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(anand): Add metrics for rejected requests.

  if (slave->state == Slave::RECOVERING) {
    return ServiceUnavailable("Agent has not finished recovery");
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  Option<string> contentType_ = request.headers.get("Content-Type");
  if (contentType_.isNone()) {
    return BadRequest("Expecting 'Content-Type' to be present");
  }

  ContentType contentType;
  if (contentType_.get() == APPLICATION_JSON) {
    contentType = ContentType::JSON;
  } else if (contentType_.get() == APPLICATION_PROTOBUF) {
    contentType = ContentType::PROTOBUF;
  } else if (contentType_.get() == APPLICATION_RECORDIO) {
    contentType = ContentType::RECORDIO;
  } else {
    return UnsupportedMediaType(
        string("Expecting 'Content-Type' of ") +
        APPLICATION_JSON + " or " + APPLICATION_PROTOBUF +
        + " or " + APPLICATION_RECORDIO);
  }

  Option<ContentType> messageContentType;
  Option<string> messageContentType_ =
    request.headers.get(MESSAGE_CONTENT_TYPE);

  if (streamingMediaType(contentType)) {
    if (messageContentType_.isNone()) {
      return BadRequest(
          "Expecting '" + stringify(MESSAGE_CONTENT_TYPE) + "' to be" +
          " set for streaming requests");
    }

    if (messageContentType_.get() == APPLICATION_JSON) {
      messageContentType = Option<ContentType>(ContentType::JSON);
    } else if (messageContentType_.get() == APPLICATION_PROTOBUF) {
      messageContentType = Option<ContentType>(ContentType::PROTOBUF);
    } else {
      return UnsupportedMediaType(
          string("Expecting '") + MESSAGE_CONTENT_TYPE + "' of " +
          APPLICATION_JSON + " or " + APPLICATION_PROTOBUF);
    }
  } else {
    // Validate that a client has not set the "Message-Content-Type"
    // header for a non-streaming request.
    if (messageContentType_.isSome()) {
      return UnsupportedMediaType(
          string("Expecting '") + MESSAGE_CONTENT_TYPE + "' to be not"
          " set for non-streaming requests");
    }
  }

  // This lambda deserializes a string into a valid `Call`
  // based on the content type.
  auto deserializer = [](const string& body, ContentType contentType)
      -> Try<mesos::agent::Call> {
    Try<v1::agent::Call> v1Call =
      deserialize<v1::agent::Call>(contentType, body);

    if (v1Call.isError()) {
      return Error(v1Call.error());
    }

    mesos::agent::Call call = devolve(v1Call.get());

    Option<Error> error = validation::agent::call::validate(call);
    if (error.isSome()) {
      return Error("Failed to validate agent::Call: " + error->message);
    }

    return call;
  };

  // For backwards compatibility, if a client does not specify an 'Accept'
  // header, 'Content-Type' of the response is set to 'application/json'
  // for streaming responses.
  //
  // TODO(anand): In v2 API, the default 'Content-Type' for streaming responses
  // should be 'application/recordio'.
  ContentType acceptType;
  if (request.acceptsMediaType(APPLICATION_JSON)) {
    acceptType = ContentType::JSON;
  } else if (request.acceptsMediaType(APPLICATION_PROTOBUF)) {
    acceptType = ContentType::PROTOBUF;
  } else if (request.acceptsMediaType(APPLICATION_RECORDIO)) {
    acceptType = ContentType::RECORDIO;
  } else {
    return NotAcceptable(
        string("Expecting 'Accept' to allow ") +
        APPLICATION_JSON + " or " + APPLICATION_PROTOBUF + " or " +
        APPLICATION_RECORDIO);
  }

  Option<ContentType> messageAcceptType;
  if (streamingMediaType(acceptType)) {
    // Note that `acceptsMediaType()` returns true if the given headers
    // field does not exist, i.e. by default we return JSON here.
    if (request.acceptsMediaType(MESSAGE_ACCEPT, APPLICATION_JSON)) {
      messageAcceptType = ContentType::JSON;
    } else if (request.acceptsMediaType(MESSAGE_ACCEPT, APPLICATION_PROTOBUF)) {
      messageAcceptType = ContentType::PROTOBUF;
    } else {
      return NotAcceptable(
          string("Expecting '") + MESSAGE_ACCEPT + "' to allow " +
          APPLICATION_JSON + " or " + APPLICATION_PROTOBUF);
    }
  } else {
    // Validate that a client has not set the "Message-Accept"
    // header for a non-streaming response.
    if (request.headers.contains(MESSAGE_ACCEPT)) {
      return NotAcceptable(
          string("Expecting '") + MESSAGE_ACCEPT +
          "' to be not set for non-streaming responses");
    }
  }

  CHECK_EQ(Request::PIPE, request.type);
  CHECK_SOME(request.reader);

  RequestMediaTypes mediaTypes {
      contentType, acceptType, messageContentType, messageAcceptType};

  if (streamingMediaType(contentType)) {
    CHECK_SOME(mediaTypes.messageContent);

    Owned<Reader<mesos::agent::Call>> reader(new Reader<mesos::agent::Call>(
        Decoder<mesos::agent::Call>(lambda::bind(
            deserializer, lambda::_1, mediaTypes.messageContent.get())),
        request.reader.get()));

    return reader->read()
      .then(defer(
          slave->self(),
          [=](const Result<mesos::agent::Call>& call) -> Future<Response> {
            if (call.isNone()) {
              return BadRequest("Received EOF while reading request body");
            }

            if (call.isError()) {
              return BadRequest(call.error());
            }

            return _api(call.get(), std::move(reader), mediaTypes, principal);
          }));
  } else {
    Pipe::Reader reader = request.reader.get();  // Remove const.

    return reader.readAll()
      .then(defer(
          slave->self(),
          [=](const string& body) -> Future<Response> {
            Try<mesos::agent::Call> call = deserializer(body, contentType);
            if (call.isError()) {
              return BadRequest(call.error());
            }
            return _api(call.get(), None(), mediaTypes, principal);
          }));
  }
}


Future<Response> Http::_api(
    const mesos::agent::Call& call,
    Option<Owned<Reader<mesos::agent::Call>>>&& reader,
    const RequestMediaTypes& mediaTypes,
    const Option<Principal>& principal) const
{
  // Validate that a client has not _accidentally_ sent us a
  // streaming request for a call type that does not support it.
  if (streamingMediaType(mediaTypes.content) &&
      call.type() != mesos::agent::Call::ATTACH_CONTAINER_INPUT) {
    return UnsupportedMediaType(
        "Streaming 'Content-Type' " + stringify(mediaTypes.content) + " is "
        "not supported for " + stringify(call.type()) + " call");
  } else if (!streamingMediaType(mediaTypes.content) &&
             call.type() == mesos::agent::Call::ATTACH_CONTAINER_INPUT) {
    return UnsupportedMediaType(
        string("Expecting 'Content-Type' to be ") + APPLICATION_RECORDIO +
        " for " + stringify(call.type()) + " call");
  }

  if (streamingMediaType(mediaTypes.accept) &&
      call.type() != mesos::agent::Call::ATTACH_CONTAINER_OUTPUT &&
      call.type() != mesos::agent::Call::LAUNCH_NESTED_CONTAINER_SESSION) {
    return NotAcceptable("Streaming response is not supported for " +
        stringify(call.type()) + " call");
  }

  // Each handler must log separately to add context
  // it might extract from the nested message.
  switch (call.type()) {
    case mesos::agent::Call::UNKNOWN:
      return NotImplemented();

    case mesos::agent::Call::GET_HEALTH:
      return getHealth(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_FLAGS:
      return getFlags(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_VERSION:
      return getVersion(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_METRICS:
      return getMetrics(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_LOGGING_LEVEL:
      return getLoggingLevel(call, mediaTypes.accept, principal);

    case mesos::agent::Call::SET_LOGGING_LEVEL:
      return setLoggingLevel(call, mediaTypes.accept, principal);

    case mesos::agent::Call::LIST_FILES:
      return listFiles(call, mediaTypes.accept, principal);

    case mesos::agent::Call::READ_FILE:
      return readFile(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_STATE:
      return getState(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_CONTAINERS:
      return getContainers(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_FRAMEWORKS:
      return getFrameworks(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_EXECUTORS:
      return getExecutors(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_OPERATIONS:
      return getOperations(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_TASKS:
      return getTasks(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_AGENT:
      return getAgent(call, mediaTypes.accept, principal);

    case mesos::agent::Call::GET_RESOURCE_PROVIDERS:
      return getResourceProviders(call, mediaTypes.accept, principal);

    case mesos::agent::Call::LAUNCH_NESTED_CONTAINER:
      return launchNestedContainer(call, mediaTypes.accept, principal);

    case mesos::agent::Call::WAIT_NESTED_CONTAINER:
      return waitNestedContainer(call, mediaTypes.accept, principal);

    case mesos::agent::Call::KILL_NESTED_CONTAINER:
      return killNestedContainer(call, mediaTypes.accept, principal);

    case mesos::agent::Call::REMOVE_NESTED_CONTAINER:
      return removeNestedContainer(call, mediaTypes.accept, principal);

    case mesos::agent::Call::LAUNCH_NESTED_CONTAINER_SESSION:
      return launchNestedContainerSession(call, mediaTypes, principal);

    case mesos::agent::Call::ATTACH_CONTAINER_INPUT:
      CHECK_SOME(reader);
      return attachContainerInput(
          call, std::move(reader).get(), mediaTypes, principal);

    case mesos::agent::Call::ATTACH_CONTAINER_OUTPUT:
      return attachContainerOutput(call, mediaTypes, principal);

    case mesos::agent::Call::LAUNCH_CONTAINER:
      return launchContainer(call, mediaTypes.accept, principal);

    case mesos::agent::Call::WAIT_CONTAINER:
      return waitContainer(call, mediaTypes.accept, principal);

    case mesos::agent::Call::KILL_CONTAINER:
      return killContainer(call, mediaTypes.accept, principal);

    case mesos::agent::Call::REMOVE_CONTAINER:
      return removeContainer(call, mediaTypes.accept, principal);

    case mesos::agent::Call::ADD_RESOURCE_PROVIDER_CONFIG:
      return addResourceProviderConfig(call, principal);

    case mesos::agent::Call::UPDATE_RESOURCE_PROVIDER_CONFIG:
      return updateResourceProviderConfig(call, principal);

    case mesos::agent::Call::REMOVE_RESOURCE_PROVIDER_CONFIG:
      return removeResourceProviderConfig(call, principal);

    case mesos::agent::Call::PRUNE_IMAGES:
      return pruneImages(call, mediaTypes.accept, principal);
  }

  UNREACHABLE();
}


string Http::EXECUTOR_HELP() {
  return HELP(
    TLDR(
        "Endpoint for the Executor HTTP API."),
    DESCRIPTION(
        "This endpoint is used by the executors to interact with the",
        "agent via Call/Event messages.",
        "",
        "Returns 200 OK iff the initial SUBSCRIBE Call is successful.",
        "This will result in a streaming response via chunked",
        "transfer encoding. The executors can process the response",
        "incrementally.",
        "",
        "Returns 202 Accepted for all other Call messages iff the",
        "request is accepted."),
    AUTHENTICATION(true));
}


// TODO(greggomann): Remove this function when implicit executor authorization
// is moved into the authorizer. See MESOS-7399.
Option<Error> verifyExecutorClaims(
    const Principal& principal,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId) {
  if (!(principal.claims.contains("fid") &&
        principal.claims.at("fid") == frameworkId.value())) {
    return Error(
        "Authenticated principal '" + stringify(principal) + "' does not "
        "contain an 'fid' claim with the framework ID " +
        stringify(frameworkId) + ", which is set in the call");
  }

  if (!(principal.claims.contains("eid") &&
        principal.claims.at("eid") == executorId.value())) {
    return Error(
        "Authenticated principal '" + stringify(principal) + "' does not "
        "contain an 'eid' claim with the executor ID " +
        stringify(executorId) + ", which is set in the call");
  }

  if (!(principal.claims.contains("cid") &&
        principal.claims.at("cid") == containerId.value())) {
    return Error(
        "Authenticated principal '" + stringify(principal) + "' does not "
        "contain a 'cid' claim with the correct active ContainerID");
  }

  return None();
}


Future<Response> Http::executor(
    const Request& request,
    const Option<Principal>& principal) const
{
  if (!slave->recoveryInfo.reconnect) {
    CHECK_EQ(slave->state, Slave::RECOVERING);
    return ServiceUnavailable("Agent has not finished recovery");
  }

  // TODO(anand): Add metrics for rejected requests.

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  v1::executor::Call v1Call;

  Option<string> contentType = request.headers.get("Content-Type");
  if (contentType.isNone()) {
    return BadRequest("Expecting 'Content-Type' to be present");
  }

  if (contentType.get() == APPLICATION_PROTOBUF) {
    if (!v1Call.ParseFromString(request.body)) {
      return BadRequest("Failed to parse body into Call protobuf");
    }
  } else if (contentType.get() == APPLICATION_JSON) {
    Try<JSON::Value> value = JSON::parse(request.body);
    if (value.isError()) {
      return BadRequest("Failed to parse body into JSON: " + value.error());
    }

    Try<v1::executor::Call> parse =
      ::protobuf::parse<v1::executor::Call>(value.get());

    if (parse.isError()) {
      return BadRequest("Failed to convert JSON into Call protobuf: " +
                        parse.error());
    }

    v1Call = parse.get();
  } else {
    return UnsupportedMediaType(
        string("Expecting 'Content-Type' of ") +
        APPLICATION_JSON + " or " + APPLICATION_PROTOBUF);
  }

  const executor::Call call = devolve(v1Call);

  Option<Error> error = common::validation::validateExecutorCall(call);

  if (error.isSome()) {
    return BadRequest("Failed to validate Executor::Call: " + error->message);
  }

  ContentType acceptType;

  if (call.type() == executor::Call::SUBSCRIBE) {
    // We default to JSON since an empty 'Accept' header
    // results in all media types considered acceptable.
    if (request.acceptsMediaType(APPLICATION_JSON)) {
      acceptType = ContentType::JSON;
    } else if (request.acceptsMediaType(APPLICATION_PROTOBUF)) {
      acceptType = ContentType::PROTOBUF;
    } else {
      return NotAcceptable(
          string("Expecting 'Accept' to allow ") +
          "'" + APPLICATION_PROTOBUF + "' or '" + APPLICATION_JSON + "'");
    }
  } else {
    if (slave->state == Slave::RECOVERING) {
      return ServiceUnavailable("Agent has not finished recovery");
    }
  }

  // We consolidate the framework/executor lookup logic here because
  // it is common for all the call handlers.
  Framework* framework = slave->getFramework(call.framework_id());
  if (framework == nullptr) {
    return BadRequest("Framework cannot be found");
  }

  Executor* executor = framework->getExecutor(call.executor_id());
  if (executor == nullptr) {
    return BadRequest("Executor cannot be found");
  }

  // TODO(greggomann): Move this implicit executor authorization
  // into the authorizer. See MESOS-7399.
  if (principal.isSome()) {
    error = verifyExecutorClaims(
        principal.get(),
        call.framework_id(),
        call.executor_id(),
        executor->containerId);

    if (error.isSome()) {
      return Forbidden(error->message);
    }
  }

  if (executor->state == Executor::REGISTERING &&
      call.type() != executor::Call::SUBSCRIBE) {
    return Forbidden("Executor is not subscribed");
  }

  switch (call.type()) {
    case executor::Call::SUBSCRIBE: {
      Pipe pipe;
      OK ok;
      ok.headers["Content-Type"] = stringify(acceptType);

      ok.type = Response::PIPE;
      ok.reader = pipe.reader();

      HttpConnection http {pipe.writer(), acceptType};
      slave->subscribe(http, call.subscribe(), framework, executor);

      return ok;
    }

    case executor::Call::UPDATE: {
      slave->statusUpdate(protobuf::createStatusUpdate(
          call.framework_id(),
          call.update().status(),
          slave->info.id()),
          None());

      return Accepted();
    }

    case executor::Call::MESSAGE: {
      slave->executorMessage(
          slave->info.id(),
          framework->id(),
          executor->id,
          call.message().data());

      return Accepted();
    }

    case executor::Call::UNKNOWN: {
      LOG(WARNING) << "Received 'UNKNOWN' call";
      return NotImplemented();
    }
  }

  UNREACHABLE();
}


string Http::RESOURCE_PROVIDER_HELP() {
  return HELP(
    TLDR(
        "Endpoint for the local resource provider HTTP API."),
    DESCRIPTION(
        "This endpoint is used by the local resource providers to interact",
        "with the agent via Call/Event messages.",
        "",
        "Returns 200 OK iff the initial SUBSCRIBE Call is successful. This",
        "will result in a streaming response via chunked transfer encoding.",
        "The local resource providers can process the response incrementally.",
        "",
        "Returns 202 Accepted for all other Call messages iff the request is",
        "accepted."),
    AUTHENTICATION(true));
}


string Http::FLAGS_HELP()
{
  return HELP(
    TLDR("Exposes the agent's flag configuration."),
    None(),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "The request principal should be authorized to view all flags.",
        "See the authorization documentation for details."));
}


Future<Response> Http::flags(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(nfnt): Remove check for enabled
  // authorization as part of MESOS-5346.
  if (request.method != "GET" && slave->authorizer.isSome()) {
    return MethodNotAllowed({"GET"}, request.method);
  }

  if (slave->authorizer.isNone()) {
    return OK(_flags(), request.url.query.get("jsonp"));
  }

  authorization::Request authRequest;
  authRequest.set_action(authorization::VIEW_FLAGS);

  Option<authorization::Subject> subject = createSubject(principal);
  if (subject.isSome()) {
    authRequest.mutable_subject()->CopyFrom(subject.get());
  }

  return slave->authorizer.get()->authorized(authRequest)
      .then(defer(
          slave->self(),
          [this, request](bool authorized) -> Future<Response> {
            if (authorized) {
              return OK(_flags(), request.url.query.get("jsonp"));
            } else {
              return Forbidden();
            }
          }));
}


JSON::Object Http::_flags() const
{
  JSON::Object object;

  {
    JSON::Object flags;
    foreachvalue (const flags::Flag& flag, slave->flags) {
      Option<string> value = flag.stringify(slave->flags);
      if (value.isSome()) {
        flags.values[flag.effective_name().value] = value.get();
      }
    }
    object.values["flags"] = std::move(flags);
  }

  return object;
}


Future<Response> Http::getFlags(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_FLAGS, call.type());

  LOG(INFO) << "Processing GET_FLAGS call";

  return ObjectApprovers::create(slave->authorizer, principal, {VIEW_FLAGS})
    .then(defer(
        slave->self(),
        [this, acceptType](
            const Owned<ObjectApprovers>& approvers) -> Response {
          if (!approvers->approved<VIEW_FLAGS>()) {
            return Forbidden();
          }

          return OK(
              serialize(
                  acceptType, evolve<v1::agent::Response::GET_FLAGS>(_flags())),
              stringify(acceptType));
        }));
}


string Http::HEALTH_HELP()
{
  return HELP(
    TLDR(
        "Health check of the Agent."),
    DESCRIPTION(
        "Returns 200 OK iff the Agent is healthy.",
        "Delayed responses are also indicative of poor health."),
    AUTHENTICATION(false));
}


Future<Response> Http::health(const Request& request) const
{
  return OK();
}


Future<Response> Http::getHealth(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_HEALTH, call.type());

  LOG(INFO) << "Processing GET_HEALTH call";

  mesos::agent::Response response;
  response.set_type(mesos::agent::Response::GET_HEALTH);
  response.mutable_get_health()->set_healthy(true);

  return OK(serialize(acceptType, evolve(response)),
            stringify(acceptType));
}


Future<Response> Http::getVersion(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_VERSION, call.type());

  LOG(INFO) << "Processing GET_VERSION call";

  return OK(serialize(acceptType,
                      evolve<v1::agent::Response::GET_VERSION>(version())),
            stringify(acceptType));
}


Future<Response> Http::getMetrics(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_METRICS, call.type());
  CHECK(call.has_get_metrics());

  LOG(INFO) << "Processing GET_METRICS call";

  Option<Duration> timeout;
  if (call.get_metrics().has_timeout()) {
    timeout = Nanoseconds(call.get_metrics().timeout().nanoseconds());
  }

  return process::metrics::snapshot(timeout)
      .then([acceptType](const map<string, double>& metrics) -> Response {
          mesos::agent::Response response;
        response.set_type(mesos::agent::Response::GET_METRICS);
        mesos::agent::Response::GetMetrics* _getMetrics =
          response.mutable_get_metrics();

        foreachpair (const string& key, double value, metrics) {
          Metric* metric = _getMetrics->add_metrics();
          metric->set_name(key);
          metric->set_value(value);
        }

        return OK(serialize(acceptType, evolve(response)),
                  stringify(acceptType));
      });
}


Future<Response> Http::getLoggingLevel(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_LOGGING_LEVEL, call.type());

  LOG(INFO) << "Processing GET_LOGGING_LEVEL call";

  mesos::agent::Response response;
  response.set_type(mesos::agent::Response::GET_LOGGING_LEVEL);
  response.mutable_get_logging_level()->set_level(FLAGS_v);

  return OK(serialize(acceptType, evolve(response)),
            stringify(acceptType));
}


Future<Response> Http::setLoggingLevel(
    const mesos::agent::Call& call,
    ContentType /*contentType*/,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::SET_LOGGING_LEVEL, call.type());
  CHECK(call.has_set_logging_level());

  uint32_t level = call.set_logging_level().level();
  Duration duration =
    Nanoseconds(call.set_logging_level().duration().nanoseconds());

  LOG(INFO) << "Processing SET_LOGGING_LEVEL call for level " << level;

  return ObjectApprovers::create(slave->authorizer, principal, {SET_LOG_LEVEL})
    .then([level, duration](
        const Owned<ObjectApprovers>& approvers) -> Future<Response> {
      if (!approvers->approved<SET_LOG_LEVEL>()) {
        return Forbidden();
      }

      return dispatch(process::logging(), &Logging::set_level, level, duration)
        .then([]() -> Response {
          return OK();
        });
    });
}


Future<Response> Http::listFiles(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::LIST_FILES, call.type());

  const string& path = call.list_files().path();

  LOG(INFO) << "Processing LIST_FILES call for path '" << path << "'";

  return slave->files->browse(path, principal)
    .then([acceptType](const Try<list<FileInfo>, FilesError>& result)
      -> Future<Response> {
      if (result.isError()) {
        const FilesError& error = result.error();

        switch (error.type) {
          case FilesError::Type::INVALID:
            return BadRequest(error.message);

          case FilesError::Type::UNAUTHORIZED:
            return Forbidden(error.message);

          case FilesError::Type::NOT_FOUND:
            return NotFound(error.message);

          case FilesError::Type::UNKNOWN:
            return InternalServerError(error.message);
        }

        UNREACHABLE();
      }

      mesos::agent::Response response;
      response.set_type(mesos::agent::Response::LIST_FILES);

      mesos::agent::Response::ListFiles* listFiles =
        response.mutable_list_files();

      foreach (const FileInfo& fileInfo, result.get()) {
        listFiles->add_file_infos()->CopyFrom(fileInfo);
      }

      return OK(serialize(acceptType, evolve(response)),
                stringify(acceptType));
    });
}


string Http::STATE_HELP() {
  return HELP(
    TLDR(
        "Information about state of the Agent."),
    DESCRIPTION(
        "This endpoint shows information about the frameworks, executors",
        "and the agent's master as a JSON object.",
        "The information shown might be filtered based on the user",
        "accessing the endpoint.",
        "",
        "Example (**Note**: this is not exhaustive):",
        "",
        "```",
        "{",
        "    \"version\" : \"0.28.0\",",
        "    \"git_sha\" : \"9d5889b5a265849886a533965f4aefefd1fbd103\",",
        "    \"git_branch\" : \"refs/heads/master\",",
        "    \"git_tag\" : \"0.28.0\",",
        "    \"build_date\" : \"2016-02-15 10:00:28\"",
        "    \"build_time\" : 1455559228,",
        "    \"build_user\" : \"mesos-user\",",
        "    \"start_time\" : 1455647422.88396,",
        "    \"id\" : \"e2c38084-f6ea-496f-bce3-b6e07cea5e01-S0\",",
        "    \"pid\" : \"slave(1)@127.0.1.1:5051\",",
        "    \"hostname\" : \"localhost\",",
        "    \"resources\" : {",
        "         \"ports\" : \"[31000-32000]\",",
        "         \"mem\" : 127816,",
        "         \"disk\" : 804211,",
        "         \"cpus\" : 32",
        "    },",
        "    \"attributes\" : {},",
        "    \"master_hostname\" : \"localhost\",",
        "    \"log_dir\" : \"/var/log\",",
        "    \"external_log_file\" : \"mesos.log\",",
        "    \"frameworks\" : [],",
        "    \"completed_frameworks\" : [],",
        "    \"flags\" : {",
        "         \"gc_disk_headroom\" : \"0.1\",",
        "         \"isolation\" : \"posix/cpu,posix/mem\",",
        "         \"containerizers\" : \"mesos\",",
        "         \"docker_socket\" : \"/var/run/docker.sock\",",
        "         \"gc_delay\" : \"1weeks\",",
        "         \"gc_non_executor_container_sandboxes\" : \"false\",",
        "         \"docker_remove_delay\" : \"6hrs\",",
        "         \"port\" : \"5051\",",
        "         \"systemd_runtime_directory\" : \"/run/systemd/system\",",
        "         \"initialize_driver_logging\" : \"true\",",
        "         \"cgroups_root\" : \"mesos\",",
        "         \"fetcher_cache_size\" : \"2GB\",",
        "         \"cgroups_hierarchy\" : \"/sys/fs/cgroup\",",
        "         \"qos_correction_interval_min\" : \"0ns\",",
        "         \"cgroups_cpu_enable_pids_and_tids_count\" : \"false\",",
        "         \"sandbox_directory\" : \"/mnt/mesos/sandbox\",",
        "         \"docker\" : \"docker\",",
        "         \"help\" : \"false\",",
        "         \"docker_stop_timeout\" : \"0ns\",",
        "         \"master\" : \"127.0.0.1:5050\",",
        "         \"logbufsecs\" : \"0\",",
        "         \"docker_registry\" : \"https://registry-1.docker.io\",",
        "         \"frameworks_home\" : \"\",",
        "         \"cgroups_enable_cfs\" : \"false\",",
        "         \"perf_interval\" : \"1mins\",",
        "         \"docker_kill_orphans\" : \"true\",",
        "         \"switch_user\" : \"true\",",
        "         \"logging_level\" : \"INFO\",",
        "         \"strict\" : \"true\",",
        "         \"executor_registration_timeout\" : \"1mins\",",
        "         \"recovery_timeout\" : \"15mins\",",
        "         \"revocable_cpu_low_priority\" : \"true\",",
        "         \"docker_store_dir\" : \"/tmp/mesos/store/docker\",",
        "         \"image_provisioner_backend\" : \"copy\",",
        "         \"authenticatee\" : \"crammd5\",",
        "         \"quiet\" : \"false\",",
        "         \"executor_shutdown_grace_period\" : \"5secs\",",
        "         \"fetcher_cache_dir\" : \"/tmp/mesos/fetch\",",
        "         \"default_role\" : \"*\",",
        "         \"work_dir\" : \"/tmp/mesos\",",
        "         \"launcher_dir\" : \"/path/to/mesos/build/src\",",
        "         \"registration_backoff_factor\" : \"1secs\",",
        "         \"oversubscribed_resources_interval\" : \"15secs\",",
        "         \"enforce_container_disk_quota\" : \"false\",",
        "         \"container_disk_watch_interval\" : \"15secs\",",
        "         \"disk_watch_interval\" : \"1mins\",",
        "         \"cgroups_limit_swap\" : \"false\",",
        "         \"hostname_lookup\" : \"true\",",
        "         \"perf_duration\" : \"10secs\",",
        "         \"appc_store_dir\" : \"/tmp/mesos/store/appc\",",
        "         \"recover\" : \"reconnect\",",
        "         \"version\" : \"false\"",
        "    },",
        "}",
        "```"),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "This endpoint might be filtered based on the user accessing it.",
        "For example a user might only see the subset of frameworks,",
        "tasks, and executors they are allowed to view.",
        "See the authorization documentation for details."));
}


Future<Response> Http::state(
    const Request& request,
    const Option<Principal>& principal) const
{
  if (slave->state == Slave::RECOVERING) {
    return ServiceUnavailable("Agent has not finished recovery");
  }

  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {VIEW_FRAMEWORK, VIEW_TASK, VIEW_EXECUTOR, VIEW_FLAGS, VIEW_ROLE})
    .then(defer(
        slave->self(),
        [this, request](const Owned<ObjectApprovers>& approvers) -> Response {
          // This lambda is consumed before the outer lambda
          // returns, hence capture by reference is fine here.
          auto state = [this, &approvers](JSON::ObjectWriter* writer) {
            writer->field("version", MESOS_VERSION);

            if (build::GIT_SHA.isSome()) {
              writer->field("git_sha", build::GIT_SHA.get());
            }

            if (build::GIT_BRANCH.isSome()) {
              writer->field("git_branch", build::GIT_BRANCH.get());
            }

            if (build::GIT_TAG.isSome()) {
              writer->field("git_tag", build::GIT_TAG.get());
            }

            writer->field("build_date", build::DATE);
            writer->field("build_time", build::TIME);
            writer->field("build_user", build::USER);
            writer->field("start_time", slave->startTime.secs());

            writer->field("id", slave->info.id().value());
            writer->field("pid", string(slave->self()));
            writer->field("hostname", slave->info.hostname());
            writer->field(
                "capabilities",
                slave->capabilities.toRepeatedPtrField());

            if (slave->info.has_domain()) {
              writer->field("domain", slave->info.domain());
            }

            const Resources& totalResources = slave->totalResources;

            writer->field("resources", totalResources);
            writer->field(
                "reserved_resources",
                [&totalResources, &approvers](JSON::ObjectWriter* writer) {
                  foreachpair (const string& role,
                               const Resources& resources,
                               totalResources.reservations()) {
                    if (approvers->approved<VIEW_ROLE>(role)) {
                      writer->field(role, resources);
                    }
                  }
                });

            writer->field("unreserved_resources", totalResources.unreserved());

            writer->field(
                "reserved_resources_full",
                [&totalResources, &approvers](JSON::ObjectWriter* writer) {
                  foreachpair (const string& role,
                               const Resources& resources,
                               totalResources.reservations()) {
                    if (approvers->approved<VIEW_ROLE>(role)) {
                      writer->field(
                          role,
                          [&resources](JSON::ArrayWriter* writer) {
                            foreach (Resource resource, resources) {
                              convertResourceFormat(&resource, ENDPOINT);
                              writer->element(JSON::Protobuf(resource));
                            }
                          });
                    }
                  }
                });

            writer->field(
                "unreserved_resources_full",
                [&totalResources](JSON::ArrayWriter* writer) {
                  foreach (Resource resource, totalResources.unreserved()) {
                    convertResourceFormat(&resource, ENDPOINT);
                    writer->element(JSON::Protobuf(resource));
                  }
                });

            // TODO(abudnik): Consider storing the allocatedResources in the
            // Slave struct rather than computing it here each time.
            Resources allocatedResources;

            foreachvalue (const Framework* framework, slave->frameworks) {
              allocatedResources += framework->allocatedResources();
            }

            writer->field(
                "reserved_resources_allocated",
                [&allocatedResources, &approvers](JSON::ObjectWriter* writer) {
                  foreachpair (const string& role,
                               const Resources& resources,
                               allocatedResources.reservations()) {
                    if (approvers->approved<VIEW_ROLE>(role)) {
                      writer->field(role, resources);
                    }
                  }
                });

            writer->field(
                "unreserved_resources_allocated",
                allocatedResources.unreserved());

            writer->field("attributes", Attributes(slave->info.attributes()));

            if (slave->master.isSome()) {
              Try<string> hostname =
                  net::getHostname(slave->master->address.ip);

              if (hostname.isSome()) {
                writer->field("master_hostname", hostname.get());
              }
            }

            if (approvers->approved<VIEW_FLAGS>()) {
              if (slave->flags.log_dir.isSome()) {
                writer->field("log_dir", slave->flags.log_dir.get());
              }

              if (slave->flags.external_log_file.isSome()) {
                writer->field(
                    "external_log_file", slave->flags.external_log_file.get());
              }

              writer->field("flags", [this](JSON::ObjectWriter* writer) {
                  foreachvalue (const flags::Flag& flag, slave->flags) {
                    Option<string> value = flag.stringify(slave->flags);
                    if (value.isSome()) {
                      writer->field(flag.effective_name().value, value.get());
                    }
                  }
                });
            }

            // Model all of the frameworks.
            writer->field(
                "frameworks",
                [this, &approvers](JSON::ArrayWriter* writer) {
                  foreachvalue (Framework* framework, slave->frameworks) {
                    // Skip unauthorized frameworks.
                    if (!approvers->approved<VIEW_FRAMEWORK>(framework->info)) {
                      continue;
                    }

                    writer->element(FrameworkWriter(approvers, framework));
                  }
                });

            // Model all of the completed frameworks.
            writer->field(
                "completed_frameworks",
            [this, &approvers](JSON::ArrayWriter* writer) {
                  foreachvalue (const Owned<Framework>& framework,
                                slave->completedFrameworks) {
                    // Skip unauthorized frameworks.
                    if (!approvers->approved<VIEW_FRAMEWORK>(framework->info)) {
                      continue;
                    }

                    writer->element(
                        FrameworkWriter(approvers, framework.get()));
                  }
                });
          };

          return OK(jsonify(state), request.url.query.get("jsonp"));
        }));
}


Future<Response> Http::getFrameworks(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_FRAMEWORKS, call.type());

  LOG(INFO) << "Processing GET_FRAMEWORKS call";

  return ObjectApprovers::create(slave->authorizer, principal, {VIEW_FRAMEWORK})
    .then(defer(
        slave->self(),
        [this, acceptType](
            const Owned<ObjectApprovers>& approvers) -> Response {
          mesos::agent::Response response;
          response.set_type(mesos::agent::Response::GET_FRAMEWORKS);
          *response.mutable_get_frameworks() = _getFrameworks(approvers);

          return OK(serialize(acceptType, evolve(response)),
                    stringify(acceptType));
        }));
}


mesos::agent::Response::GetFrameworks Http::_getFrameworks(
    const Owned<ObjectApprovers>& approvers) const
{
  mesos::agent::Response::GetFrameworks getFrameworks;
  foreachvalue (const Framework* framework, slave->frameworks) {
    // Skip unauthorized frameworks.
    if (!approvers->approved<VIEW_FRAMEWORK>(framework->info)) {
      continue;
    }

    getFrameworks.add_frameworks()->mutable_framework_info()
      ->CopyFrom(framework->info);
  }

  foreachvalue (const Owned<Framework>& framework, slave->completedFrameworks) {
    // Skip unauthorized frameworks.
    if (!approvers->approved<VIEW_FRAMEWORK>(framework->info)) {
      continue;
    }

    getFrameworks.add_completed_frameworks()->mutable_framework_info()
      ->CopyFrom(framework->info);
  }

  return getFrameworks;
}


Future<Response> Http::getExecutors(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_EXECUTORS, call.type());

  LOG(INFO) << "Processing GET_EXECUTORS call";

  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {VIEW_FRAMEWORK, VIEW_EXECUTOR})
    .then(defer(
        slave->self(),
        [this, acceptType](
            const Owned<ObjectApprovers>& approvers) -> Response {
          mesos::agent::Response response;
          response.set_type(mesos::agent::Response::GET_EXECUTORS);

          *response.mutable_get_executors() = _getExecutors(approvers);

          return OK(serialize(acceptType, evolve(response)),
                    stringify(acceptType));
        }));
}


mesos::agent::Response::GetExecutors Http::_getExecutors(
    const Owned<ObjectApprovers>& approvers) const
{
  // Construct framework list with both active and completed frameworks.
  vector<const Framework*> frameworks;
  foreachvalue (Framework* framework, slave->frameworks) {
    // Skip unauthorized frameworks.
    if (!approvers->approved<VIEW_FRAMEWORK>(framework->info)) {
      continue;
    }

    frameworks.push_back(framework);
  }

  foreachvalue (const Owned<Framework>& framework, slave->completedFrameworks) {
    // Skip unauthorized frameworks.
    if (!approvers->approved<VIEW_FRAMEWORK>(framework->info)) {
      continue;
    }

    frameworks.push_back(framework.get());
  }

  mesos::agent::Response::GetExecutors getExecutors;

  foreach (const Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      // Skip unauthorized executors.
      if (!approvers->approved<VIEW_EXECUTOR>(
              executor->info,
              framework->info)) {
        continue;
      }

      getExecutors.add_executors()->mutable_executor_info()->CopyFrom(
          executor->info);
    }

    foreach (const Owned<Executor>& executor, framework->completedExecutors) {
      // Skip unauthorized executors.
      if (!approvers->approved<VIEW_EXECUTOR>(
              executor->info,
              framework->info)) {
        continue;
      }

      getExecutors.add_completed_executors()->mutable_executor_info()->CopyFrom(
          executor->info);
    }
  }

  return getExecutors;
}


Future<Response> Http::getOperations(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_OPERATIONS, call.type());

  LOG(INFO) << "Processing GET_OPERATIONS call";

  return ObjectApprovers::create(slave->authorizer, principal, {VIEW_ROLE})
    .then(defer(
        slave->self(),
        [=](const Owned<ObjectApprovers>& approvers) -> Response {
          // We consider a principal to be authorized to view an operation if it
          // is authorized to view the resources the operation is performed on.
          auto approved = [&approvers](const Operation& operation) {
            Try<Resources> consumedResources =
              protobuf::getConsumedResources(operation.info());

            if (consumedResources.isError()) {
              LOG(WARNING)
                << "Could not approve operation " << operation.uuid()
                << " since its consumed resources could not be determined: "
                << consumedResources.error();

              return false;
            }

            foreach (const Resource& resource, consumedResources.get()) {
              if (!approvers->approved<VIEW_ROLE>(resource)) {
                return false;
              }
            }

            return true;
          };

          agent::Response response;
          response.set_type(mesos::agent::Response::GET_OPERATIONS);

          agent::Response::GetOperations* operations =
            response.mutable_get_operations();

          foreachvalue (Operation* operation, slave->operations) {
            if (approved(*operation)) {
              operations->add_operations()->CopyFrom(*operation);
            }
          }

          return OK(
              serialize(acceptType, evolve(response)), stringify(acceptType));
        }));
}


Future<Response> Http::getTasks(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_TASKS, call.type());

  LOG(INFO) << "Processing GET_TASKS call";

  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {VIEW_FRAMEWORK, VIEW_TASK, VIEW_EXECUTOR})
    .then(defer(
        slave->self(),
        [this, acceptType](
            const Owned<ObjectApprovers>& approvers) -> Response {
          mesos::agent::Response response;
          response.set_type(mesos::agent::Response::GET_TASKS);

          *response.mutable_get_tasks() = _getTasks(approvers);

          return OK(serialize(acceptType, evolve(response)),
                    stringify(acceptType));
        }));
}


mesos::agent::Response::GetTasks Http::_getTasks(
    const Owned<ObjectApprovers>& approvers) const
{
  // Construct framework list with both active and completed frameworks.
  vector<const Framework*> frameworks;
  foreachvalue (Framework* framework, slave->frameworks) {
    // Skip unauthorized frameworks.
    if (!approvers->approved<VIEW_FRAMEWORK>(framework->info)) {
      continue;
    }

    frameworks.push_back(framework);
  }

  foreachvalue (const Owned<Framework>& framework, slave->completedFrameworks) {
    // Skip unauthorized frameworks.
    if (!approvers->approved<VIEW_FRAMEWORK>(framework->info)) {
      continue;
    }

    frameworks.push_back(framework.get());
  }

  // Construct executor list with both active and completed executors.
  hashmap<const Executor*, const Framework*> executors;
  foreach (const Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      // Skip unauthorized executors.
      if (!approvers->approved<VIEW_EXECUTOR>(
              executor->info, framework->info)) {
        continue;
      }

      executors.put(executor, framework);
    }

    foreach (const Owned<Executor>& executor, framework->completedExecutors) {
      // Skip unauthorized executors.
      if (!approvers->approved<VIEW_EXECUTOR>(
              executor->info, framework->info)) {
        continue;
      }

      executors.put(executor.get(), framework);
    }
  }

  mesos::agent::Response::GetTasks getTasks;

  foreach (const Framework* framework, frameworks) {
    // Pending tasks.
    typedef hashmap<TaskID, TaskInfo> TaskMap;
    foreachvalue (const TaskMap& taskInfos, framework->pendingTasks) {
      foreachvalue (const TaskInfo& taskInfo, taskInfos) {
        // Skip unauthorized tasks.
        if (!approvers->approved<VIEW_TASK>(taskInfo, framework->info)) {
          continue;
        }

        const Task& task =
          protobuf::createTask(taskInfo, TASK_STAGING, framework->id());

        getTasks.add_pending_tasks()->CopyFrom(task);
      }
    }
  }

  foreachpair (const Executor* executor,
               const Framework* framework,
               executors) {
    // Queued tasks.
    foreachvalue (const TaskInfo& taskInfo, executor->queuedTasks) {
      // Skip unauthorized tasks.
      if (!approvers->approved<VIEW_TASK>(taskInfo, framework->info)) {
        continue;
      }

      const Task& task =
        protobuf::createTask(taskInfo, TASK_STAGING, framework->id());

      getTasks.add_queued_tasks()->CopyFrom(task);
    }

    // Launched tasks.
    foreachvalue (Task* task, executor->launchedTasks) {
      CHECK_NOTNULL(task);
      // Skip unauthorized tasks.
      if (!approvers->approved<VIEW_TASK>(*task, framework->info)) {
        continue;
      }

      getTasks.add_launched_tasks()->CopyFrom(*task);
    }

    // Terminated tasks.
    foreachvalue (Task* task, executor->terminatedTasks) {
      CHECK_NOTNULL(task);
      // Skip unauthorized tasks.
      if (!approvers->approved<VIEW_TASK>(*task, framework->info)) {
        continue;
      }

      getTasks.add_terminated_tasks()->CopyFrom(*task);
    }

    // Completed tasks.
    foreach (const std::shared_ptr<Task>& task, executor->completedTasks) {
      // Skip unauthorized tasks.
      if (!approvers->approved<VIEW_TASK>(*task.get(), framework->info)) {
        continue;
      }

      getTasks.add_completed_tasks()->CopyFrom(*task);
    }
  }

  return getTasks;
}


Future<Response> Http::getAgent(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_AGENT, call.type());

  LOG(INFO) << "Processing GET_AGENT call";

  agent::Response response;
  response.set_type(mesos::agent::Response::GET_AGENT);

  response.mutable_get_agent()->mutable_slave_info()->CopyFrom(slave->info);

  return OK(serialize(acceptType, evolve(response)),
            stringify(acceptType));
}


Future<Response> Http::getResourceProviders(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_RESOURCE_PROVIDERS, call.type());

  LOG(INFO) << "Processing GET_RESOURCE_PROVIDERS call";

  return ObjectApprovers::create(
      slave->authorizer, principal, {VIEW_RESOURCE_PROVIDER})
    .then(defer(
        slave->self(),
        [this, acceptType](
            const Owned<ObjectApprovers>& approvers) -> Response {
          agent::Response response;
          response.set_type(mesos::agent::Response::GET_RESOURCE_PROVIDERS);

          agent::Response::GetResourceProviders* resourceProviders =
            response.mutable_get_resource_providers();

          foreachvalue (
              ResourceProvider* resourceProvider, slave->resourceProviders) {
            if (!approvers->approved<VIEW_RESOURCE_PROVIDER>()) {
              continue;
            }
            agent::Response::GetResourceProviders::ResourceProvider* provider =
              resourceProviders->add_resource_providers();

            provider->mutable_resource_provider_info()->CopyFrom(
                resourceProvider->info);

            provider->mutable_total_resources()->CopyFrom(
                resourceProvider->totalResources);
          }

          return OK(
              serialize(acceptType, evolve(response)), stringify(acceptType));
        }));
}


Future<Response> Http::getState(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_STATE, call.type());

  LOG(INFO) << "Processing GET_STATE call";

  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {VIEW_FRAMEWORK, VIEW_TASK, VIEW_EXECUTOR})
    .then(defer(
        slave->self(),
        [=](const Owned<ObjectApprovers>& approvers) -> Response {
          mesos::agent::Response response;
          response.set_type(mesos::agent::Response::GET_STATE);
          *response.mutable_get_state() = _getState(approvers);

          return OK(serialize(acceptType, evolve(response)),
                    stringify(acceptType));
        }));
}


mesos::agent::Response::GetState Http::_getState(
    const Owned<ObjectApprovers>& approvers) const
{
  mesos::agent::Response::GetState getState;

  *getState.mutable_get_tasks() = _getTasks(approvers);
  *getState.mutable_get_executors() = _getExecutors(approvers);
  *getState.mutable_get_frameworks() = _getFrameworks(approvers);

  return getState;
}


string Http::STATISTICS_HELP()
{
  return HELP(
      TLDR(
          "Retrieve resource monitoring information."),
      DESCRIPTION(
          "Returns the current resource consumption data for containers",
          "running under this agent.",
          "",
          "Example:",
          "",
          "```",
          "[{",
          "    \"executor_id\":\"executor\",",
          "    \"executor_name\":\"name\",",
          "    \"framework_id\":\"framework\",",
          "    \"source\":\"source\",",
          "    \"statistics\":",
          "    {",
          "        \"cpus_limit\":8.25,",
          "        \"cpus_nr_periods\":769021,",
          "        \"cpus_nr_throttled\":1046,",
          "        \"cpus_system_time_secs\":34501.45,",
          "        \"cpus_throttled_time_secs\":352.597023453,",
          "        \"cpus_user_time_secs\":96348.84,",
          "        \"mem_anon_bytes\":4845449216,",
          "        \"mem_file_bytes\":260165632,",
          "        \"mem_limit_bytes\":7650410496,",
          "        \"mem_mapped_file_bytes\":7159808,",
          "        \"mem_rss_bytes\":5105614848,",
          "        \"timestamp\":1388534400.0",
          "    }",
          "}]",
          "```"),
      AUTHENTICATION(true),
      AUTHORIZATION(
          "The request principal should be authorized to query this endpoint.",
          "See the authorization documentation for details."));
}


Future<Response> Http::statistics(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(nfnt): Remove check for enabled
  // authorization as part of MESOS-5346.
  if (request.method != "GET" && slave->authorizer.isSome()) {
    return MethodNotAllowed({"GET"}, request.method);
  }

  Try<string> endpoint = extractEndpoint(request.url);
  if (endpoint.isError()) {
    return Failure("Failed to extract endpoint: " + endpoint.error());
  }

  return authorizeEndpoint(
      endpoint.get(),
      request.method,
      slave->authorizer,
      principal)
    .then(defer(
        slave->self(),
        [this, request](bool authorized) -> Future<Response> {
          if (!authorized) {
            return Forbidden();
          }

          return statisticsLimiter->acquire()
            .then(defer(slave->self(), &Slave::usage))
            .then(defer(slave->self(),
                  [this, request](const ResourceUsage& usage) {
              return _statistics(usage, request);
            }));
        }));
}


Response Http::_statistics(
    const ResourceUsage& usage,
    const Request& request) const
{
  JSON::Array result;

  foreach (const ResourceUsage::Executor& executor, usage.executors()) {
    if (executor.has_statistics()) {
      const ExecutorInfo& info = executor.executor_info();

      JSON::Object entry;
      entry.values["framework_id"] = info.framework_id().value();
      entry.values["executor_id"] = info.executor_id().value();
      entry.values["executor_name"] = info.name();
      entry.values["source"] = info.source();
      entry.values["statistics"] = JSON::protobuf(executor.statistics());

      result.values.push_back(entry);
    }
  }

  return OK(result, request.url.query.get("jsonp"));
}


string Http::CONTAINERS_HELP()
{
  return HELP(
      TLDR(
          "Retrieve container status and usage information."),
      DESCRIPTION(
          "Returns the current resource consumption data and status for",
          "containers running under this slave.",
          "",
          "Example (**Note**: this is not exhaustive):",
          "",
          "```",
          "[{",
          "    \"container_id\":\"container\",",
          "    \"container_status\":",
          "    {",
          "        \"network_infos\":",
          "        [{\"ip_addresses\":[{\"ip_address\":\"192.168.1.1\"}]}]",
          "    }",
          "    \"executor_id\":\"executor\",",
          "    \"executor_name\":\"name\",",
          "    \"framework_id\":\"framework\",",
          "    \"source\":\"source\",",
          "    \"statistics\":",
          "    {",
          "        \"cpus_limit\":8.25,",
          "        \"cpus_nr_periods\":769021,",
          "        \"cpus_nr_throttled\":1046,",
          "        \"cpus_system_time_secs\":34501.45,",
          "        \"cpus_throttled_time_secs\":352.597023453,",
          "        \"cpus_user_time_secs\":96348.84,",
          "        \"mem_anon_bytes\":4845449216,",
          "        \"mem_file_bytes\":260165632,",
          "        \"mem_limit_bytes\":7650410496,",
          "        \"mem_mapped_file_bytes\":7159808,",
          "        \"mem_rss_bytes\":5105614848,",
          "        \"timestamp\":1388534400.0",
          "    }",
          "}]",
          "```"),
      AUTHENTICATION(true),
      AUTHORIZATION(
          "The request principal should be authorized to query this endpoint.",
          "See the authorization documentation for details."));
}


Future<Response> Http::containers(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(a10gupta): Remove check for enabled
  // authorization as part of MESOS-5346.
  if (request.method != "GET" && slave->authorizer.isSome()) {
    return MethodNotAllowed({"GET"}, request.method);
  }

  Try<string> endpoint = extractEndpoint(request.url);
  if (endpoint.isError()) {
    return Failure("Failed to extract endpoint: " + endpoint.error());
  }

  return authorizeEndpoint(
      endpoint.get(),
      request.method,
      slave->authorizer,
      principal)
    .then(defer(
        slave->self(),
        [this, request, principal](bool authorized) -> Future<Response> {
          if (!authorized) {
            return Forbidden();
          }

          return _containers(request, principal);
        }));
}


Future<Response> Http::getContainers(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::GET_CONTAINERS, call.type());

  LOG(INFO) << "Processing GET_CONTAINERS call";

  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {VIEW_CONTAINER, VIEW_STANDALONE_CONTAINER})
    .then(defer(
        slave->self(),
        [this, call](const Owned<ObjectApprovers>& approvers) {
          // Use an empty container ID filter.
          return __containers(
              approvers,
              None(),
              call.get_containers().show_nested(),
              call.get_containers().show_standalone());
        }))
    .then([acceptType](const JSON::Array& result) -> Response {
      return OK(
          serialize(
              acceptType,
              evolve<v1::agent::Response::GET_CONTAINERS>(result)),
          stringify(acceptType));
    });
}


Future<Response> Http::_containers(
    const Request& request,
    const Option<Principal>& principal) const
{
  Option<string> containerId = request.url.query.get("container_id");

  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {VIEW_CONTAINER, VIEW_STANDALONE_CONTAINER})
    .then(defer(
        slave->self(),
        [this, containerId](const Owned<ObjectApprovers>& approvers) {
          IDAcceptor<ContainerID> selectContainerId(containerId);

          return __containers(
              approvers,
              selectContainerId,
              false,
              false);
        }))
    .then([request](const JSON::Array& result) -> Response {
      return process::http::OK(result, request.url.query.get("jsonp"));
    });
}


Future<JSON::Array> Http::__containers(
    const Owned<ObjectApprovers>& approvers,
    Option<IDAcceptor<ContainerID>> selectContainerId,
    bool showNestedContainers,
    bool showStandaloneContainers) const
{
  return slave->containerizer->containers()
    .then(defer(slave->self(), [=](const hashset<ContainerID> containerIds) {
      Owned<vector<JSON::Object>> metadata(new vector<JSON::Object>());
      vector<Future<ContainerStatus>> statusFutures;
      vector<Future<ResourceStatistics>> statsFutures;

      hashset<ContainerID> executorContainerIds;
      hashset<ContainerID> authorizedExecutorContainerIds;

      foreachvalue (const Framework* framework, slave->frameworks) {
        foreachvalue (const Executor* executor, framework->executors) {
          // No need to get statistics and status if we know that the
          // executor has already terminated.
          if (executor->state == Executor::TERMINATED) {
            continue;
          }

          const ExecutorInfo& info = executor->info;
          const ContainerID& containerId = executor->containerId;

          executorContainerIds.insert(containerId);

          if ((selectContainerId.isSome() &&
               !selectContainerId->accept(containerId)) ||
              !approvers->approved<VIEW_CONTAINER>(info, framework->info)) {
            continue;
          }

          authorizedExecutorContainerIds.insert(containerId);

          JSON::Object entry;
          entry.values["framework_id"] = info.framework_id().value();
          entry.values["executor_id"] = info.executor_id().value();
          entry.values["executor_name"] = info.name();
          entry.values["source"] = info.source();
          entry.values["container_id"] = containerId.value();

          metadata->push_back(entry);
          statusFutures.push_back(slave->containerizer->status(containerId));
          statsFutures.push_back(slave->containerizer->usage(containerId));
        }
      }

      foreach (const ContainerID& containerId, containerIds) {
        if (executorContainerIds.contains(containerId)) {
          continue;
        }

        if (selectContainerId.isSome() &&
            !selectContainerId->accept(containerId)) {
          continue;
        }

        const bool isNestedContainer = containerId.has_parent();

        // TODO(jieyu): Only MesosContainerizer supports standalone
        // container currently. Thus it's ok to call
        // MesosContainerizer-specific method here. If we want to
        // support other Containerizers, we should make this a
        // Containerizer interface.
        const bool isStandaloneContainer =
          containerizer::paths::isStandaloneContainer(
              slave->flags.runtime_dir,
              containerId);

        // For nested containers, authorization is always based on
        // its root container.
        ContainerID rootContainerId = protobuf::getRootContainerId(containerId);

        const bool isRootContainerStandalone =
          containerizer::paths::isStandaloneContainer(
              slave->flags.runtime_dir,
              rootContainerId);

        if (isNestedContainer && !showNestedContainers) {
          continue;
        }

        if (isStandaloneContainer && !showStandaloneContainers) {
          continue;
        }

        if (isRootContainerStandalone &&
            !approvers->approved<VIEW_STANDALONE_CONTAINER>(rootContainerId)) {
          continue;
        }

        if (!isRootContainerStandalone &&
            !authorizedExecutorContainerIds.contains(rootContainerId)) {
          continue;
        }

        JSON::Object entry;
        entry.values["container_id"] = containerId.value();

        metadata->push_back(entry);
        statusFutures.push_back(slave->containerizer->status(containerId));
        statsFutures.push_back(slave->containerizer->usage(containerId));
      }

      return await(await(statusFutures), await(statsFutures)).then(
          [metadata](const tuple<
              Future<vector<Future<ContainerStatus>>>,
              Future<vector<Future<ResourceStatistics>>>>& t)
              -> Future<JSON::Array> {
            const vector<Future<ContainerStatus>>& status =
              std::get<0>(t).get();

            const vector<Future<ResourceStatistics>>& stats =
              std::get<1>(t).get();

            CHECK_EQ(status.size(), stats.size());
            CHECK_EQ(status.size(), metadata->size());

            JSON::Array result;

            auto statusIter = status.begin();
            auto statsIter = stats.begin();
            auto metadataIter = metadata->begin();

            while (statusIter != status.end() &&
                   statsIter != stats.end() &&
                   metadataIter != metadata->end()) {
              JSON::Object& entry = *metadataIter;

              if (statusIter->isReady()) {
                entry.values["status"] = JSON::protobuf(statusIter->get());
              } else {
                LOG(WARNING) << "Failed to get container status for executor '"
                             << entry.values["executor_id"] << "'"
                             << " of framework "
                             << entry.values["framework_id"] << ": "
                             << (statusIter->isFailed()
                                  ? statusIter->failure()
                                  : "discarded");
              }

              if (statsIter->isReady()) {
                entry.values["statistics"] = JSON::protobuf(statsIter->get());
              } else {
                LOG(WARNING)
                  << "Failed to get resource statistics for executor '"
                  << entry.values["executor_id"] << "'"
                  << " of framework "
                  << entry.values["framework_id"] << ": "
                  << (statsIter->isFailed()
                      ? statsIter->failure()
                      : "discarded");
              }

              result.values.push_back(entry);

              statusIter++;
              statsIter++;
              metadataIter++;
            }

            return result;
          });
    }));
}


Future<Response> Http::pruneImages(
    const agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(agent::Call::PRUNE_IMAGES, call.type());

  LOG(INFO) << "Processing PRUNE_IMAGES call";
  vector<Image> excludedImages(call.prune_images().excluded_images().begin(),
                               call.prune_images().excluded_images().end());

  // Include any `excluded_images` from agent flag's `image_gc_config`
  // if not empty.
  if (slave->flags.image_gc_config.isSome()) {
    std::copy(slave->flags.image_gc_config->excluded_images().begin(),
              slave->flags.image_gc_config->excluded_images().end(),
              std::back_inserter(excludedImages));
  }

  return ObjectApprovers::create(slave->authorizer, principal, {PRUNE_IMAGES})
    .then(defer(
        slave->self(),
        [this, excludedImages](
            const Owned<ObjectApprovers>& approvers) -> Future<Response> {
          if (!approvers->approved<PRUNE_IMAGES>()) {
            return Forbidden();
          }

          return slave->containerizer->pruneImages(excludedImages)
            .then([]() -> Response { return OK(); });
        }));
}


Try<string> Http::extractEndpoint(const process::http::URL& url) const
{
  // Paths are of the form "/slave(n)/endpoint". We're only interested
  // in the part after "/slave(n)" and tokenize the path accordingly.
  //
  // TODO(alexr): In the long run, absolute paths for
  // endpoins should be supported, see MESOS-5369.
  const vector<string> pathComponents = strings::tokenize(url.path, "/", 2);

  if (pathComponents.size() < 2u ||
      pathComponents[0] != slave->self().id) {
    return Error("Unexpected path '" + url.path + "'");
  }

  return "/" + pathComponents[1];
}


Future<Response> Http::readFile(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::READ_FILE, call.type());

  const size_t offset = call.read_file().offset();
  const string& path = call.read_file().path();

  LOG(INFO) << "Processing READ_FILE call for path '" << path << "'";

  Option<size_t> length;
  if (call.read_file().has_length()) {
    length = call.read_file().length();
  }

  return slave->files->read(offset, length, path, principal)
    .then([acceptType](const Try<tuple<size_t, string>, FilesError>& result)
        -> Future<Response> {
      if (result.isError()) {
        const FilesError& error = result.error();

        switch (error.type) {
          case FilesError::Type::INVALID:
            return BadRequest(error.message);

          case FilesError::Type::UNAUTHORIZED:
            return Forbidden(error.message);

          case FilesError::Type::NOT_FOUND:
            return NotFound(error.message);

          case FilesError::Type::UNKNOWN:
            return InternalServerError(error.message);
        }

        UNREACHABLE();
      }

      mesos::agent::Response response;
      response.set_type(mesos::agent::Response::READ_FILE);

      response.mutable_read_file()->set_size(std::get<0>(result.get()));
      response.mutable_read_file()->set_data(std::get<1>(result.get()));

      return OK(serialize(acceptType, evolve(response)),
                stringify(acceptType));
    });
}


Future<Response> Http::launchNestedContainer(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::LAUNCH_NESTED_CONTAINER, call.type());
  CHECK(call.has_launch_nested_container());

  LOG(INFO) << "Processing LAUNCH_NESTED_CONTAINER call for container '"
            << call.launch_nested_container().container_id() << "'";

  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {LAUNCH_NESTED_CONTAINER})
    .then(defer(
        slave->self(),
        [=](const Owned<ObjectApprovers>& authorizer) {
          return _launchContainer<LAUNCH_NESTED_CONTAINER>(
              call.launch_nested_container().container_id(),
              call.launch_nested_container().command(),
              None(),
              call.launch_nested_container().has_container()
                ? call.launch_nested_container().container()
                : Option<ContainerInfo>::none(),
              ContainerClass::DEFAULT,
              acceptType,
              authorizer);
        }));
}


Future<Response> Http::launchContainer(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::LAUNCH_CONTAINER, call.type());
  CHECK(call.has_launch_container());

  LOG(INFO) << "Processing LAUNCH_CONTAINER call for container '"
            << call.launch_container().container_id() << "'";

  if (call.launch_container().container_id().has_parent()) {
    return launchContainer<LAUNCH_NESTED_CONTAINER>(
        call,
        acceptType,
        principal);
  }

  return launchContainer<LAUNCH_STANDALONE_CONTAINER>(
      call,
      acceptType,
      principal);
}


template <authorization::Action action>
Future<Response> Http::launchContainer(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {action})
    .then(defer(
        slave->self(),
        [=](const Owned<ObjectApprovers>& approvers) -> Future<Response> {
          return _launchContainer<action>(
              call.launch_container().container_id(),
              call.launch_container().command(),
              call.launch_container().resources(),
              call.launch_container().has_container()
                ? call.launch_container().container()
                : Option<ContainerInfo>::none(),
              ContainerClass::DEFAULT,
              acceptType,
              approvers);
        }));
}


template <authorization::Action action>
Future<Response> Http::_launchContainer(
    const ContainerID& containerId,
    const CommandInfo& commandInfo,
    const Option<Resources>& resources,
    const Option<ContainerInfo>& containerInfo,
    const Option<ContainerClass>& containerClass,
    ContentType,
    const Owned<ObjectApprovers>& approvers) const
{
  // Attempt to get the executor associated with this ContainerID.
  // We only expect to get the executor when launching a nested container
  // under a container launched via a scheduler. In other cases, we are
  // launching a standalone container (possibly nested).
  Executor* executor = slave->getExecutor(containerId);
  if (executor == nullptr) {
    if (!approvers->approved<action>(containerId)) {
      return Forbidden();
    }
  } else {
    Framework* framework = slave->getFramework(executor->frameworkId);
    CHECK_NOTNULL(framework);

    if (!approvers->approved<action>(
            executor->info, framework->info, commandInfo, containerId)) {
      return Forbidden();
    }
  }

  ContainerConfig containerConfig;
  containerConfig.mutable_command_info()->CopyFrom(commandInfo);

#ifndef __WINDOWS__
  if (slave->flags.switch_user && commandInfo.has_user()) {
    containerConfig.set_user(commandInfo.user());
  }
#endif // __WINDOWS__

  if (resources.isSome()) {
    containerConfig.mutable_resources()->CopyFrom(resources.get());
  }

  if (containerInfo.isSome()) {
    containerConfig.mutable_container_info()->CopyFrom(containerInfo.get());
  }

  if (containerClass.isSome()) {
    containerConfig.set_container_class(containerClass.get());
  }

  // For standalone top-level containers, supply a sandbox directory.
  if (!containerId.has_parent()) {
    const string directory =
      slave::paths::getContainerPath(slave->flags.work_dir, containerId);

    if (containerConfig.has_user()) {
      LOG_BASED_ON_CLASS(containerConfig.container_class())
        << "Creating sandbox '" << directory << "'"
        << " for user '" << containerConfig.user() << "'";
    } else {
      LOG_BASED_ON_CLASS(containerConfig.container_class())
        << "Creating sandbox '" << directory << "'";
    }

    Try<Nothing> mkdir = slave::paths::createSandboxDirectory(
        directory,
        containerConfig.has_user() ? Option<string>(containerConfig.user())
                                   : Option<string>::none());

    if (mkdir.isError()){
      return InternalServerError("Failed to create sandbox: " + mkdir.error());
    }

    containerConfig.set_directory(directory);
  }

  Future<Containerizer::LaunchResult> launched = slave->containerizer->launch(
      containerId,
      containerConfig,
      map<string, string>(),
      None());

  // TODO(jieyu): If the http connection breaks, the handler will
  // trigger a discard on the returned future. That'll result in the
  // future 'launched' transitioning into DISCARDED state. However,
  // this does not mean the launch was discarded correctly and it
  // requires a destroy. See MESOS-8039 for more details.
  //
  // TODO(bmahler): The containerizers currently require that
  // the caller calls destroy if the launch fails. See MESOS-6214.
  launched.onAny(defer(
      slave->self(),
      [=](const Future<Containerizer::LaunchResult>& launchResult) {
        if (launchResult.isReady()) {
          return;
        }

        LOG(WARNING)
          << "Failed to launch container " << containerId << ": "
          << (launchResult.isFailed() ? launchResult.failure() : "discarded");

        slave->containerizer->destroy(containerId)
          .onAny([=](const Future<Option<ContainerTermination>>& destroy) {
            if (destroy.isReady()) {
              return;
            }

            LOG(ERROR)
              << "Failed to destroy container " << containerId
              << " after launch failure: "
              << (destroy.isFailed() ? destroy.failure() : "discarded");
          });
      }));

  return launched
    .then([](const Containerizer::LaunchResult launchResult) -> Response {
      switch (launchResult) {
        case Containerizer::LaunchResult::SUCCESS:
          return OK();
        case Containerizer::LaunchResult::ALREADY_LAUNCHED:
          return Accepted();
        case Containerizer::LaunchResult::NOT_SUPPORTED:
          return BadRequest("The provided ContainerInfo is not supported");

        // NOTE: By not setting a default we leverage the compiler
        // errors when the enumeration is augmented to find all
        // the cases we need to provide.
      }

      UNREACHABLE();
    })
    .repair([](const Future<Response>& launch) {
      // NOTE: Failures are automatically translated into 500 Internal Server
      // Errors, but a launch failure is likely due to user input.
      return BadRequest(launch.failure());
    });
}


Future<Response> Http::waitNestedContainer(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::WAIT_NESTED_CONTAINER, call.type());
  CHECK(call.has_wait_nested_container());

  LOG(INFO) << "Processing WAIT_NESTED_CONTAINER call for container '"
            << call.wait_nested_container().container_id() << "'";

  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {WAIT_NESTED_CONTAINER})
    .then(defer(
        slave->self(),
        [=](const Owned<ObjectApprovers>& approvers) {
          return _waitContainer<WAIT_NESTED_CONTAINER>(
              call.wait_nested_container().container_id(),
              acceptType,
              approvers,
              true);
        }));
}


Future<Response> Http::waitContainer(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::WAIT_CONTAINER, call.type());
  CHECK(call.has_wait_container());

  LOG(INFO) << "Processing WAIT_CONTAINER call for container '"
            << call.wait_container().container_id() << "'";

  if (call.wait_container().container_id().has_parent()) {
    return waitContainer<WAIT_NESTED_CONTAINER>(call, acceptType, principal);
  }

  return waitContainer<WAIT_STANDALONE_CONTAINER>(call, acceptType, principal);
}


template <authorization::Action action>
Future<Response> Http::waitContainer(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {action})
    .then(defer(
        slave->self(),
        [=](const Owned<ObjectApprovers>& approvers) {
          return _waitContainer<action>(
              call.wait_container().container_id(),
              acceptType,
              approvers,
              false);
        }));
}


template <authorization::Action action>
Future<Response> Http::_waitContainer(
    const ContainerID& containerId,
    ContentType acceptType,
    const Owned<ObjectApprovers>& approvers,
    const bool deprecated) const
{
  // Attempt to get the executor associated with this ContainerID.
  // We only expect to get the executor when waiting upon a nested container
  // under a container launched via a scheduler. In other cases, we are
  // waiting on a standalone container (possibly nested).
  Executor* executor = slave->getExecutor(containerId);
  if (executor == nullptr) {
    if (!approvers->approved<action>(containerId)) {
      return Forbidden();
    }
  } else {
    Framework* framework = slave->getFramework(executor->frameworkId);
    CHECK_NOTNULL(framework);

    if (!approvers->approved<action>(
            executor->info, framework->info, containerId)) {
      return Forbidden();
    }
  }

  return slave->containerizer->wait(containerId)
    .then([=](const Option<ContainerTermination>& termination) -> Response {
      if (termination.isNone()) {
        return NotFound(
            "Container " + stringify(containerId) + " cannot be found");
      }

      mesos::agent::Response response;

      // The response object depends on which API was originally used
      // to make this call.
      if (deprecated) {
        response.set_type(mesos::agent::Response::WAIT_NESTED_CONTAINER);

        mesos::agent::Response::WaitNestedContainer* waitNestedContainer =
          response.mutable_wait_nested_container();

        if (termination->has_status()) {
          waitNestedContainer->set_exit_status(termination->status());
        }

        if (termination->has_state()) {
          waitNestedContainer->set_state(termination->state());
        }

        if (termination->has_reason()) {
          waitNestedContainer->set_reason(termination->reason());
        }

        if (!termination->limited_resources().empty()) {
          waitNestedContainer->mutable_limitation()->mutable_resources()
            ->CopyFrom(termination->limited_resources());
        }

        if (termination->has_message()) {
          waitNestedContainer->set_message(termination->message());
        }
      } else {
        response.set_type(mesos::agent::Response::WAIT_CONTAINER);

        mesos::agent::Response::WaitContainer* waitContainer =
          response.mutable_wait_container();

        if (termination->has_status()) {
          waitContainer->set_exit_status(termination->status());
        }

        if (termination->has_state()) {
          waitContainer->set_state(termination->state());
        }

        if (termination->has_reason()) {
          waitContainer->set_reason(termination->reason());
        }

        if (!termination->limited_resources().empty()) {
          waitContainer->mutable_limitation()->mutable_resources()
            ->CopyFrom(termination->limited_resources());
        }

        if (termination->has_message()) {
          waitContainer->set_message(termination->message());
        }
      }

      return OK(serialize(acceptType, evolve(response)),
                stringify(acceptType));
    });
}


Future<Response> Http::killNestedContainer(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::KILL_NESTED_CONTAINER, call.type());
  CHECK(call.has_kill_nested_container());

  LOG(INFO) << "Processing KILL_NESTED_CONTAINER call for container '"
            << call.kill_nested_container().container_id() << "'";

  // SIGKILL is used by default if a signal is not specified.
  int signal = SIGKILL;
  if (call.kill_nested_container().has_signal()) {
    signal = call.kill_nested_container().signal();
  }

  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {KILL_NESTED_CONTAINER})
    .then(defer(
        slave->self(),
        [=](const Owned<ObjectApprovers>& approvers) {
          return _killContainer<KILL_NESTED_CONTAINER>(
              call.kill_nested_container().container_id(),
              signal,
              acceptType,
              approvers);
        }));
}


Future<Response> Http::killContainer(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::KILL_CONTAINER, call.type());
  CHECK(call.has_kill_container());

  LOG(INFO) << "Processing KILL_CONTAINER call for container '"
            << call.kill_container().container_id() << "'";

  if (call.kill_container().container_id().has_parent()) {
    return killContainer<KILL_NESTED_CONTAINER>(call, acceptType, principal);
  }

  return killContainer<KILL_STANDALONE_CONTAINER>(call, acceptType, principal);
}


template <mesos::authorization::Action action>
Future<Response> Http::killContainer(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  // SIGKILL is used by default if a signal is not specified.
  int signal = SIGKILL;
  if (call.kill_container().has_signal()) {
    signal = call.kill_container().signal();
  }

  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {action})
    .then(defer(
        slave->self(),
        [=](const Owned<ObjectApprovers>& approvers) {
          return _killContainer<action>(
              call.kill_container().container_id(),
              signal,
              acceptType,
              approvers);
        }));
}


template <mesos::authorization::Action action>
Future<Response> Http::_killContainer(
    const ContainerID& containerId,
    const int signal,
    ContentType acceptType,
    const Owned<ObjectApprovers>& approvers) const
{
  // Attempt to get the executor associated with this ContainerID.
  // We only expect to get the executor when killing a nested container
  // under a container launched via a scheduler. In other cases, we are
  // killing a standalone container (possibly nested).
  Executor* executor = slave->getExecutor(containerId);
  if (executor == nullptr) {
    if (!approvers->approved<action>(containerId)) {
      return Forbidden();
    }
  } else {
    Framework* framework = slave->getFramework(executor->frameworkId);
    CHECK_NOTNULL(framework);

    if (!approvers->approved<action>(
            executor->info,
            framework->info,
            containerId)) {
      return Forbidden();
    }
  }

  Future<bool> kill = slave->containerizer->kill(containerId, signal);

  return kill
    .then([containerId](bool found) -> Response {
      if (!found) {
        return NotFound("Container '" + stringify(containerId) + "'"
                        " cannot be found (or is already killed)");
      }
      return OK();
    });
}


Future<Response> Http::removeNestedContainer(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::REMOVE_NESTED_CONTAINER, call.type());
  CHECK(call.has_remove_nested_container());

  LOG(INFO) << "Processing REMOVE_NESTED_CONTAINER call for container '"
            << call.remove_nested_container().container_id() << "'";

  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {REMOVE_NESTED_CONTAINER})
    .then(defer(
      slave->self(),
      [=](const Owned<ObjectApprovers>& approvers) {
        return _removeContainer<REMOVE_NESTED_CONTAINER>(
            call.remove_nested_container().container_id(),
            acceptType,
            approvers);
      }));
}


Future<Response> Http::removeContainer(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::REMOVE_CONTAINER, call.type());
  CHECK(call.has_remove_container());

  LOG(INFO) << "Processing REMOVE_CONTAINER call for container '"
            << call.remove_container().container_id() << "'";

  if (call.remove_container().container_id().has_parent()) {
    return removeContainer<REMOVE_NESTED_CONTAINER>(
        call,
        acceptType,
        principal);
  }

  return removeContainer<REMOVE_STANDALONE_CONTAINER>(
      call,
      acceptType,
      principal);
}


template <mesos::authorization::Action action>
Future<Response> Http::removeContainer(
    const mesos::agent::Call& call,
    ContentType acceptType,
    const Option<Principal>& principal) const
{
  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {action})
    .then(defer(
        slave->self(),
        [=](const Owned<ObjectApprovers>& approvers) {
          return _removeContainer<action>(
              call.remove_container().container_id(),
              acceptType,
              approvers);
        }));
}


template <mesos::authorization::Action action>
Future<Response> Http::_removeContainer(
    const ContainerID& containerId,
    ContentType acceptType,
    const Owned<ObjectApprovers>& approvers) const
{
  // Attempt to get the executor associated with this ContainerID.
  // We only expect to get the executor when removing a nested container
  // under a container launched via a scheduler. In other cases, we are
  // removing a standalone container (possibly nested).
  Executor* executor = slave->getExecutor(containerId);
  if (executor == nullptr) {
    if (!approvers->approved<action>(containerId)) {
      return Forbidden();
    }
  } else {
    Framework* framework = slave->getFramework(executor->frameworkId);
    CHECK_NOTNULL(framework);

    if (!approvers->approved<action>(
            executor->info,
            framework->info,
            containerId)) {
      return Forbidden();
    }
  }

  Future<Nothing> remove = slave->containerizer->remove(containerId);

  return remove
    .then([=]() -> Response { return OK(); });
}


Future<Response> Http::_attachContainerInput(
    const mesos::agent::Call& call,
    Owned<Reader<mesos::agent::Call>>&& decoder,
    const RequestMediaTypes& mediaTypes) const
{
  const ContainerID& containerId = call.attach_container_input().container_id();

  Pipe pipe;
  Pipe::Reader reader = pipe.reader();
  Pipe::Writer writer = pipe.writer();

  CHECK_SOME(mediaTypes.messageContent);
  auto encoder = [mediaTypes](const mesos::agent::Call& call) {
    ::recordio::Encoder<mesos::agent::Call> encoder(lambda::bind(
        serialize, mediaTypes.messageContent.get(), lambda::_1));

    return encoder.encode(call);
  };

  // Write the first record. We had extracted it from the `decoder`
  // in the `api()` handler to identify the call type earlier.
  pipe.writer().write(encoder(call));

  // We create this here since C++11 does not support move capture of `reader`.
  Future<Nothing> transform = recordio::transform<mesos::agent::Call>(
      std::move(decoder), encoder, writer);

  return slave->containerizer->attach(containerId)
    .then(defer(slave->self(), [=](
        Connection connection) mutable -> Future<Response> {
      Request request;
      request.method = "POST";
      request.type = Request::PIPE;
      request.reader = reader;
      request.headers = {{"Content-Type", stringify(mediaTypes.content)},
                         {MESSAGE_CONTENT_TYPE,
                             stringify(mediaTypes.messageContent.get())},
                         {"Accept", stringify(mediaTypes.accept)}};

      // See comments in `attachContainerOutput()` for the reasoning
      // behind these values.
      request.url.domain = "";
      request.url.path = "/";

      transform
        .onAny([writer](
            const Future<Nothing>& future) mutable {
          CHECK(!future.isDiscarded());

          if (future.isFailed()) {
            writer.fail(future.failure());
            return;
          }

          writer.close();
         });

      // This is a non Keep-Alive request which means the connection
      // will be closed when the response is received. Since the
      // 'Connection' is reference-counted, we must maintain a copy
      // until the disconnection occurs.
      connection.disconnected()
        .onAny([connection]() {});

      return connection.send(request)
        .onAny(defer(
            slave->self(),
            [=](const Future<Response>&) {
              // After we have received a response for `ATTACH_CONTAINER_INPUT`
              // call, we need to send an acknowledgment to the IOSwitchboard,
              // so that the IOSwitchboard process can terminate itself. This is
              // a workaround for the problem with dropping outstanding HTTP
              // responses due to a lack of graceful shutdown in libprocess.
              acknowledgeContainerInputResponse(containerId)
                .onFailed([containerId](const string& failure) {
                  LOG(ERROR) << "Failed to send an acknowledgment to the"
                             << " IOSwitchboard for container '"
                             << containerId << "': " << failure;
              });
            }));
    }));
}


Future<Response> Http::acknowledgeContainerInputResponse(
    const ContainerID& containerId) const {
  return slave->containerizer->attach(containerId)
    .then([](Connection connection) {
      Request request;
      request.method = "POST";
      request.type = Request::BODY;
      request.url.domain = "";
      request.url.path = "/acknowledge_container_input_response";

      // This is a non Keep-Alive request which means the connection
      // will be closed when the response is received. Since the
      // 'Connection' is reference-counted, we must maintain a copy
      // until the disconnection occurs.
      connection.disconnected()
        .onAny([connection]() {});

      return connection.send(request);
    });
}


Future<Response> Http::attachContainerInput(
    const mesos::agent::Call& call,
    Owned<Reader<mesos::agent::Call>>&& decoder,
    const RequestMediaTypes& mediaTypes,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::ATTACH_CONTAINER_INPUT, call.type());
  CHECK(call.has_attach_container_input());

  if (call.attach_container_input().type() !=
      mesos::agent::Call::AttachContainerInput::CONTAINER_ID) {
    return BadRequest(
        "Expecting 'attach_container_input.type' to be CONTAINER_ID");
  }

  CHECK(call.attach_container_input().has_container_id());

  LOG(INFO) << "Processing ATTACH_CONTAINER_INPUT call for container '"
            << call.attach_container_input().container_id() << "'";

  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {ATTACH_CONTAINER_INPUT})
    .then(defer(
        slave->self(),
        [this, call, decoder, mediaTypes](
            const Owned<ObjectApprovers>& approvers) -> Future<Response> {
          const ContainerID& containerId =
              call.attach_container_input().container_id();

          Executor* executor = slave->getExecutor(containerId);
          if (executor == nullptr) {
            return NotFound(
                "Container " + stringify(containerId) + " cannot be found");
          }

          Framework* framework = slave->getFramework(executor->frameworkId);
          CHECK_NOTNULL(framework);

          if (!approvers->approved<ATTACH_CONTAINER_INPUT>(
                  executor->info,
                  framework->info)) {
            return Forbidden();
          }

          Owned<Reader<mesos::agent::Call>> decoder_ = decoder;

          return _attachContainerInput(
              call, std::move(decoder_), mediaTypes);
        }));
}


Future<Response> Http::addResourceProviderConfig(
    const mesos::agent::Call& call,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::ADD_RESOURCE_PROVIDER_CONFIG, call.type());
  CHECK(call.has_add_resource_provider_config());

  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {MODIFY_RESOURCE_PROVIDER_CONFIG})
    .then(defer(
        slave->self(),
        [=](const Owned<ObjectApprovers>& approvers) -> Future<Response> {
          if (!approvers->approved<MODIFY_RESOURCE_PROVIDER_CONFIG>()) {
            return Forbidden();
          }

          const ResourceProviderInfo& info =
              call.add_resource_provider_config().info();

          LOG(INFO)
              << "Processing ADD_RESOURCE_PROVIDER_CONFIG call with type '"
              << info.type() << "' and name '" << info.name() << "'";

          Option<Error> error = LocalResourceProvider::validate(info);
          if (error.isSome()) {
            return BadRequest(
                "Failed to validate resource provider config with type '" +
                info.type() + "' and name '" + info.name() + "': " +
                error->message);
          }

          return slave->localResourceProviderDaemon->add(info)
            .then([](bool added) -> Response {
              if (!added) {
                return Conflict();
              }

              return OK();
            });
        }));
}


Future<Response> Http::updateResourceProviderConfig(
    const mesos::agent::Call& call,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::UPDATE_RESOURCE_PROVIDER_CONFIG, call.type());
  CHECK(call.has_update_resource_provider_config());

  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {MODIFY_RESOURCE_PROVIDER_CONFIG})
    .then(defer(
        slave->self(),
        [=](const Owned<ObjectApprovers>& approvers) -> Future<Response> {
          if (!approvers->approved<MODIFY_RESOURCE_PROVIDER_CONFIG>()) {
            return Forbidden();
          }

          const ResourceProviderInfo& info =
              call.update_resource_provider_config().info();

          LOG(INFO)
              << "Processing UPDATE_RESOURCE_PROVIDER_CONFIG call with type '"
              << info.type() << "' and name '" << info.name() << "'";

          Option<Error> error = LocalResourceProvider::validate(info);
          if (error.isSome()) {
            return BadRequest(
                "Failed to validate resource provider config with type '" +
                info.type() + "' and name '" + info.name() + "': " +
                error->message);
          }

          return slave->localResourceProviderDaemon->update(info)
            .then([](bool updated) -> Response {
              if (!updated) {
                return NotFound();
              }

              return OK();
            });
        }));
}


Future<Response> Http::removeResourceProviderConfig(
    const mesos::agent::Call& call,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::REMOVE_RESOURCE_PROVIDER_CONFIG, call.type());
  CHECK(call.has_remove_resource_provider_config());

  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {MODIFY_RESOURCE_PROVIDER_CONFIG})
    .then(defer(
        slave->self(),
        [=](const Owned<ObjectApprovers>& approvers) -> Future<Response> {
          if (!approvers->approved<MODIFY_RESOURCE_PROVIDER_CONFIG>()) {
            return Forbidden();
          }

          const string& type = call.remove_resource_provider_config().type();
          const string& name = call.remove_resource_provider_config().name();

          LOG(INFO)
              << "Processing REMOVE_RESOURCE_PROVIDER_CONFIG call with type '"
              << type << "' and name '" << name << "'";

          return slave->localResourceProviderDaemon->remove(type, name)
            .then([]() -> Response { return OK(); });
      }));
}


// Helper that reads data from `writer` and writes to `reader`.
// Returns a failed future if there are any errors reading or writing.
// The future is satisfied when we get a EOF.
// TODO(vinod): Move this to libprocess if this is more generally useful.
Future<Nothing> connect(Pipe::Reader reader, Pipe::Writer writer)
{
  return loop(
      None(),
      [=]() mutable {
        return reader.read();
      },
      [=](const string& chunk) mutable -> Future<ControlFlow<Nothing>> {
        if (chunk.empty()) {
          // EOF case.
          return Break();
        }

        if (!writer.write(chunk)) {
          return Failure("Write failed to the pipe");
        }

        return Continue();
      });
}


Future<Response> Http::launchNestedContainerSession(
    const mesos::agent::Call& call,
    const RequestMediaTypes& mediaTypes,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::LAUNCH_NESTED_CONTAINER_SESSION, call.type());
  CHECK(call.has_launch_nested_container_session());

  LOG(INFO) << "Processing LAUNCH_NESTED_CONTAINER_SESSION call for container '"
            << call.launch_nested_container_session().container_id() << "'";

  // Helper to destroy the container.
  auto destroy = [this](const ContainerID& containerId) {
    slave->containerizer->destroy(containerId)
      .onFailed([containerId](const string& failure) {
        LOG(ERROR) << "Failed to destroy nested container "
                   << containerId << ": " << failure;
      });
  };

  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {LAUNCH_NESTED_CONTAINER_SESSION})
    .then(defer(
      slave->self(),
      [=](const Owned<ObjectApprovers>& approvers) {
        return _launchContainer<LAUNCH_NESTED_CONTAINER_SESSION>(
            call.launch_nested_container_session().container_id(),
            call.launch_nested_container_session().command(),
            None(),
            call.launch_nested_container_session().has_container()
              ? call.launch_nested_container_session().container()
              : Option<ContainerInfo>::none(),
            ContainerClass::DEBUG,
            mediaTypes.accept,
            approvers);
      }))
    .then(defer(
      slave->self(),
      [=](const Response& response) -> Future<Response> {
        // If `response` has failed or is not `OK`, the container will be
        // destroyed by `_launchContainer`.
        const ContainerID& containerId =
            call.launch_nested_container_session().container_id();

        if (response.status != OK().status) {
          return response;
        }

        // If launch is successful, attach to the container output.
        mesos::agent::Call call;
        call.set_type(mesos::agent::Call::ATTACH_CONTAINER_OUTPUT);
        call.mutable_attach_container_output()->mutable_container_id()
            ->CopyFrom(containerId);

        // Instead of directly returning the response of `attachContainerOutput`
        // to the client, we use a level of indirection to make sure the
        // container is destroyed when the client connection breaks.
        return attachContainerOutput(call, mediaTypes, principal)
          .then(defer(
              slave->self(),
              [=](const Response& response) -> Future<Response> {
                if (response.status != OK().status) {
                  LOG(WARNING) << "Failed to attach to nested container "
                               << containerId << ": '" << response.status
                                << "' (" << response.body << ")";

                  destroy(containerId);
                  return response;
                }

                Pipe pipe;
                Pipe::Writer writer = pipe.writer();

                OK ok;
                ok.headers = response.headers; // Reuse headers from response.
                ok.type = Response::PIPE;
                ok.reader = pipe.reader();

                CHECK_EQ(Response::PIPE, response.type);
                CHECK_SOME(response.reader);
                Pipe::Reader reader = response.reader.get();

                // Read from the `response` pipe and write to
                // the client's response pipe.
                // NOTE: Need to cast the lambda to std::function here because
                // of a limitation of `defer`; `defer` does not work with
                // `mutable` lambda.
                std::function<void (const Future<Nothing>&)> _connect =
                    [=](const Future<Nothing>& future) mutable {
                      CHECK(!future.isDiscarded());

                      if (future.isFailed()) {
                        LOG(WARNING) << "Failed to send attach response for "
                                     << containerId << ": " << future.failure();

                        writer.fail(future.failure());
                        reader.close();
                      } else {
                        // EOF case.
                        LOG(INFO) << "Received EOF attach response for "
                                  << containerId;

                        writer.close();
                        reader.close();
                      }

                      destroy(containerId);
                };

                connect(reader, writer).onAny(defer(slave->self(), _connect));

                // Destroy the container if the connection to client is closed.
                writer.readerClosed()
                  .onAny(defer(
                      slave->self(),
                      [=](const Future<Nothing>& future) {
                        LOG(WARNING)
                            << "Launch nested container session connection"
                            << " for container " << containerId << " closed"
                            << (future.isFailed()
                                ? ": " + future.failure()
                                : "");

                        destroy(containerId);
                      }));

                return ok;
              }))
          .onFailed(defer(
              slave->self(),
              [=](const string& failure) {
                LOG(WARNING) << "Failed to attach to nested container "
                             << containerId << ": " << failure;

                destroy(containerId);
              }));
      }));
}


Future<Response> Http::_attachContainerOutput(
    const mesos::agent::Call& call,
    const RequestMediaTypes& mediaTypes) const
{
  const ContainerID& containerId =
    call.attach_container_output().container_id();

  return slave->containerizer->attach(containerId)
    .then([call, mediaTypes](Connection connection)
        -> Future<Response> {
      Request request;
      request.method = "POST";
      request.headers = {{"Accept", stringify(mediaTypes.accept)},
                         {"Content-Type", stringify(mediaTypes.content)}};

      // If a client sets the 'Accept' header expecting a streaming response,
      // `messageAccept` would always be set and we use it as the value of
      // 'Message-Accept' header.
      if (streamingMediaType(mediaTypes.accept)) {
        CHECK_SOME(mediaTypes.messageAccept);
        request.headers[MESSAGE_ACCEPT] =
          stringify(mediaTypes.messageAccept.get());
      }

      // The 'HOST' header must be EMPTY for non Internet addresses.
      // TODO(vinod): Instead of setting domain to empty string (which results
      // in an empty HOST header), add a new URL constructor that doesn't
      // require domain or IP.
      request.url.domain = "";

      // NOTE: The path is currently ignored by the switch board.
      request.url.path = "/";

      request.type = Request::BODY;
      request.body = serialize(mediaTypes.content, call);

      // We capture `connection` here to ensure that it doesn't go
      // out of scope until the `onAny` handler on `transform` is executed.
      return connection.send(request, true)
        .then([connection, mediaTypes](const Response& response)
            -> Future<Response> {
          if (response.status != OK().status) {
            return response;
          }

          // Evolve the `ProcessIO` records in the Response body to v1
          // before sending them to the client.
          Pipe pipe;
          Pipe::Writer writer = pipe.writer();

          OK ok;
          ok.headers = response.headers; // Reuse headers from response.

          // If a client sets the 'Accept' header expecting a streaming
          // response, `messageAccept` would always be set and we use it
          // to deserialize/evolve messages in the streaming response.
          ContentType messageContentType = mediaTypes.accept;
          if (streamingMediaType(mediaTypes.accept)) {
            CHECK_SOME(mediaTypes.messageAccept);
            messageContentType = mediaTypes.messageAccept.get();
          }

          ok.type = Response::PIPE;
          ok.reader = pipe.reader();

          CHECK_EQ(Response::PIPE, response.type);
          CHECK_SOME(response.reader);
          Pipe::Reader reader = response.reader.get();

          auto deserializer = lambda::bind(
              deserialize<ProcessIO>, messageContentType, lambda::_1);

          Owned<Reader<ProcessIO>> decoder(new Reader<ProcessIO>(
              Decoder<ProcessIO>(deserializer), reader));

          auto encoder = [messageContentType](const ProcessIO& processIO) {
            ::recordio::Encoder<v1::agent::ProcessIO> encoder (lambda::bind(
                serialize, messageContentType, lambda::_1));

            return encoder.encode(evolve(processIO));
          };

          recordio::transform<ProcessIO>(std::move(decoder), encoder, writer)
            .onAny([writer, reader, connection](
                const Future<Nothing>& future) mutable {
              CHECK(!future.isDiscarded());

              if (future.isFailed()) {
                writer.fail(future.failure());
                reader.close();
                return;
              }

              writer.close();
              reader.close();
            });

          return ok;
        });
    });
}


Future<Response> Http::attachContainerOutput(
    const mesos::agent::Call& call,
    const RequestMediaTypes& mediaTypes,
    const Option<Principal>& principal) const
{
  CHECK_EQ(mesos::agent::Call::ATTACH_CONTAINER_OUTPUT, call.type());
  CHECK(call.has_attach_container_output());

  LOG(INFO) << "Processing ATTACH_CONTAINER_OUTPUT call for container '"
            << call.attach_container_output().container_id() << "'";
  return ObjectApprovers::create(
      slave->authorizer,
      principal,
      {ATTACH_CONTAINER_OUTPUT})
    .then(defer(
        slave->self(),
        [=](const Owned<ObjectApprovers>& approvers) -> Future<Response> {
          const ContainerID& containerId =
              call.attach_container_output().container_id();

          Executor* executor = slave->getExecutor(containerId);
          if (executor == nullptr) {
            return NotFound(
                "Container " + stringify(containerId) + " cannot be found");
          }

          Framework* framework = slave->getFramework(executor->frameworkId);
          CHECK_NOTNULL(framework);

          if (!approvers->approved<ATTACH_CONTAINER_OUTPUT>(
                  executor->info, framework->info, containerId)) {
            return Forbidden();
          }

          return _attachContainerOutput(call, mediaTypes);
      }));
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
