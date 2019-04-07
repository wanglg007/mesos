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

#include <gtest/gtest.h>

#include <mesos/http.hpp>
#include <mesos/resources.hpp>

#include <mesos/state/in_memory.hpp>
#include <mesos/state/leveldb.hpp>
#include <mesos/state/state.hpp>

#include <mesos/v1/mesos.hpp>

#include <mesos/v1/resource_provider/resource_provider.hpp>

#include <process/clock.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/gtest.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/recordio.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"
#include "common/recordio.hpp"

#include "internal/devolve.hpp"

#include "resource_provider/manager.hpp"
#include "resource_provider/registrar.hpp"

#include "slave/slave.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

namespace http = process::http;

using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::state::InMemoryStorage;
using mesos::state::State;
using mesos::state::Storage;

using mesos::resource_provider::AdmitResourceProvider;
using mesos::resource_provider::Registrar;
using mesos::resource_provider::RemoveResourceProvider;

using mesos::v1::resource_provider::Call;
using mesos::v1::resource_provider::Event;

using process::Clock;
using process::Future;
using process::Message;
using process::Owned;

using process::http::Accepted;
using process::http::BadRequest;
using process::http::OK;
using process::http::UnsupportedMediaType;

using std::string;
using std::vector;

using testing::DoAll;
using testing::Eq;
using testing::Invoke;
using testing::SaveArg;
using testing::Values;
using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

class ResourceProviderManagerHttpApiTest
  : public MesosTest,
    public WithParamInterface<ContentType> {};


// The tests are parameterized by the content type of the request.
INSTANTIATE_TEST_CASE_P(
    ContentType,
    ResourceProviderManagerHttpApiTest,
    Values(ContentType::PROTOBUF, ContentType::JSON));


TEST_F(ResourceProviderManagerHttpApiTest, NoContentType)
{
  http::Request request;
  request.method = "POST";
  request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);

  ResourceProviderManager manager(
      Registrar::create(Owned<Storage>(new InMemoryStorage)).get());

  Future<http::Response> response = manager.api(request, None());

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(
      "Expecting 'Content-Type' to be present",
      response);
}


// This test sends a valid JSON blob that cannot be deserialized
// into a valid protobuf resulting in a BadRequest.
TEST_F(ResourceProviderManagerHttpApiTest, ValidJsonButInvalidProtobuf)
{
  JSON::Object object;
  object.values["string"] = "valid_json";

  http::Request request;
  request.method = "POST";
  request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  request.headers["Accept"] = APPLICATION_JSON;
  request.headers["Content-Type"] = APPLICATION_JSON;
  request.body = stringify(object);

  ResourceProviderManager manager(
      Registrar::create(Owned<Storage>(new InMemoryStorage)).get());

  Future<http::Response> response = manager.api(request, None());

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(
      "Failed to validate resource_provider::Call: "
      "Expecting 'type' to be present",
      response);
}


TEST_P(ResourceProviderManagerHttpApiTest, MalformedContent)
{
  const ContentType contentType = GetParam();

  http::Request request;
  request.method = "POST";
  request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  request.headers["Accept"] = stringify(contentType);
  request.headers["Content-Type"] = stringify(contentType);
  request.body = "MALFORMED_CONTENT";

  ResourceProviderManager manager(
      Registrar::create(Owned<Storage>(new InMemoryStorage)).get());

  Future<http::Response> response = manager.api(request, None());

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  switch (contentType) {
    case ContentType::PROTOBUF:
      AWAIT_EXPECT_RESPONSE_BODY_EQ(
          "Failed to parse body into Call protobuf",
          response);
      break;
    case ContentType::JSON:
      AWAIT_EXPECT_RESPONSE_BODY_EQ(
          "Failed to parse body into JSON: "
          "syntax error at line 1 near: MALFORMED_CONTENT",
          response);
      break;
    case ContentType::RECORDIO:
      break;
  }
}


TEST_P(ResourceProviderManagerHttpApiTest, UnsupportedContentMediaType)
{
  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();

  mesos::v1::ResourceProviderInfo* info =
    subscribe->mutable_resource_provider_info();

  info->set_type("org.apache.mesos.rp.test");
  info->set_name("test");

  const ContentType contentType = GetParam();
  const string unknownMediaType = "application/unknown-media-type";

  http::Request request;
  request.method = "POST";
  request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  request.headers["Accept"] = stringify(contentType);
  request.headers["Content-Type"] = unknownMediaType;
  request.body = serialize(contentType, call);

  ResourceProviderManager manager(
      Registrar::create(Owned<Storage>(new InMemoryStorage)).get());

  Future<http::Response> response = manager.api(request, None());

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(UnsupportedMediaType().status, response);
}


TEST_P(ResourceProviderManagerHttpApiTest, UpdateState)
{
  const ContentType contentType = GetParam();

  ResourceProviderManager manager(
      Registrar::create(Owned<Storage>(new InMemoryStorage)).get());

  Option<id::UUID> streamId;
  Option<mesos::v1::ResourceProviderID> resourceProviderId;

  // First, subscribe to the manager to get the ID.
  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();

    mesos::v1::ResourceProviderInfo* info =
      subscribe->mutable_resource_provider_info();

    info->set_type("org.apache.mesos.rp.test");
    info->set_name("test");

    http::Request request;
    request.method = "POST";
    request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    request.headers["Accept"] = stringify(contentType);
    request.headers["Content-Type"] = stringify(contentType);
    request.body = serialize(contentType, call);

    Future<http::Response> response = manager.api(request, None());

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    ASSERT_EQ(http::Response::PIPE, response->type);

    ASSERT_TRUE(response->headers.contains("Mesos-Stream-Id"));
    Try<id::UUID> uuid =
      id::UUID::fromString(response->headers.at("Mesos-Stream-Id"));

    CHECK_SOME(uuid);
    streamId = uuid.get();

    Option<http::Pipe::Reader> reader = response->reader;
    ASSERT_SOME(reader);

    recordio::Reader<Event> responseDecoder(
        ::recordio::Decoder<Event>(
            lambda::bind(deserialize<Event>, contentType, lambda::_1)),
        reader.get());

    Future<Result<Event>> event = responseDecoder.read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());

    // Check event type is subscribed and the resource provider id is set.
    ASSERT_EQ(Event::SUBSCRIBED, event->get().type());

    resourceProviderId = event->get().subscribed().provider_id();

    EXPECT_FALSE(resourceProviderId->value().empty());
  }

  // Then, update the total resources to the manager.
  {
    std::vector<v1::Resource> resources =
      v1::Resources::fromString("disk:4").get();
    foreach (v1::Resource& resource, resources) {
      resource.mutable_provider_id()->CopyFrom(resourceProviderId.get());
    }

    Call call;
    call.set_type(Call::UPDATE_STATE);
    call.mutable_resource_provider_id()->CopyFrom(resourceProviderId.get());

    Call::UpdateState* updateState = call.mutable_update_state();

    updateState->mutable_resources()->CopyFrom(v1::Resources(resources));
    updateState->mutable_resource_version_uuid()->CopyFrom(
        evolve(protobuf::createUUID()));

    http::Request request;
    request.method = "POST";
    request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    request.headers["Accept"] = stringify(contentType);
    request.headers["Content-Type"] = stringify(contentType);
    request.headers["Mesos-Stream-Id"] = stringify(streamId.get());
    request.body = serialize(contentType, call);

    Future<http::Response> response = manager.api(request, None());

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

    // The manager will send out a message informing its subscriber
    // about the newly added resources.
    Future<ResourceProviderMessage> message = manager.messages().get();

    AWAIT_READY(message);

    EXPECT_EQ(ResourceProviderMessage::Type::UPDATE_STATE, message->type);
    ASSERT_TRUE(message->updateState->info.has_id());
    EXPECT_EQ(
        devolve(resourceProviderId.get()),
        message->updateState->info.id());
    EXPECT_EQ(devolve(resources), message->updateState->totalResources);
  }
}


TEST_P(ResourceProviderManagerHttpApiTest, UpdateOperationStatus)
{
  const ContentType contentType = GetParam();

  ResourceProviderManager manager(
      Registrar::create(Owned<Storage>(new InMemoryStorage)).get());

  Option<id::UUID> streamId;
  Option<mesos::v1::ResourceProviderID> resourceProviderId;

  // First, subscribe to the manager to get the ID.
  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();

    mesos::v1::ResourceProviderInfo* info =
      subscribe->mutable_resource_provider_info();

    info->set_type("org.apache.mesos.rp.test");
    info->set_name("test");

    http::Request request;
    request.method = "POST";
    request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    request.headers["Accept"] = stringify(contentType);
    request.headers["Content-Type"] = stringify(contentType);
    request.body = serialize(contentType, call);

    Future<http::Response> response = manager.api(request, None());

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    ASSERT_EQ(http::Response::PIPE, response->type);

    ASSERT_TRUE(response->headers.contains("Mesos-Stream-Id"));
    Try<id::UUID> uuid =
      id::UUID::fromString(response->headers.at("Mesos-Stream-Id"));

    CHECK_SOME(uuid);
    streamId = uuid.get();

    Option<http::Pipe::Reader> reader = response->reader;
    ASSERT_SOME(reader);

    recordio::Reader<Event> responseDecoder(
        ::recordio::Decoder<Event>(
            lambda::bind(deserialize<Event>, contentType, lambda::_1)),
        reader.get());

    Future<Result<Event>> event = responseDecoder.read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());

    // Check event type is subscribed and the resource provider id is set.
    ASSERT_EQ(Event::SUBSCRIBED, event->get().type());

    resourceProviderId = event->get().subscribed().provider_id();

    EXPECT_FALSE(resourceProviderId->value().empty());
  }

  ASSERT_SOME(resourceProviderId);

  // Then, send an operation status update to the manager.
  {
    v1::FrameworkID frameworkId;
    frameworkId.set_value("foo");

    mesos::v1::OperationStatus status;
    status.set_state(mesos::v1::OperationState::OPERATION_FINISHED);
    status.mutable_resource_provider_id()->CopyFrom(resourceProviderId.get());

    mesos::v1::UUID operationUUID = evolve(protobuf::createUUID());;

    Call call;
    call.set_type(Call::UPDATE_OPERATION_STATUS);
    call.mutable_resource_provider_id()->CopyFrom(resourceProviderId.get());

    Call::UpdateOperationStatus* updateOperationStatus =
      call.mutable_update_operation_status();
    updateOperationStatus->mutable_framework_id()->CopyFrom(frameworkId);
    updateOperationStatus->mutable_status()->CopyFrom(status);
    updateOperationStatus->mutable_operation_uuid()->CopyFrom(operationUUID);

    http::Request request;
    request.method = "POST";
    request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    request.headers["Accept"] = stringify(contentType);
    request.headers["Content-Type"] = stringify(contentType);
    request.headers["Mesos-Stream-Id"] = stringify(streamId.get());
    request.body = serialize(contentType, call);

    Future<http::Response> response = manager.api(request, None());

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);

    // The manager will send out a message informing its subscriber
    // about the updated operation.
    Future<ResourceProviderMessage> message = manager.messages().get();

    AWAIT_READY(message);

    ASSERT_EQ(
        ResourceProviderMessage::Type::UPDATE_OPERATION_STATUS,
        message->type);
    EXPECT_EQ(
        devolve(frameworkId),
        message->updateOperationStatus->update.framework_id());
    EXPECT_EQ(
        devolve(status).state(),
        message->updateOperationStatus->update.status().state());
    EXPECT_EQ(
        operationUUID.value(),
        message->updateOperationStatus->update.operation_uuid().value());
  }
}


// This test verifies that the pending future returned by
// `ResourceProviderManager::publishResources()` becomes ready when the manager
// receives an publish status update with an `OK` status.
TEST_P(ResourceProviderManagerHttpApiTest, PublishResourcesSuccess)
{
  const ContentType contentType = GetParam();

  ResourceProviderManager manager(
      Registrar::create(Owned<Storage>(new InMemoryStorage)).get());

  Option<id::UUID> streamId;
  Option<mesos::v1::ResourceProviderID> resourceProviderId;
  Owned<recordio::Reader<Event>> responseDecoder;

  // First, subscribe to the manager to get the ID.
  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();

    mesos::v1::ResourceProviderInfo* info =
      subscribe->mutable_resource_provider_info();

    info->set_type("org.apache.mesos.rp.test");
    info->set_name("test");

    http::Request request;
    request.method = "POST";
    request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    request.headers["Accept"] = stringify(contentType);
    request.headers["Content-Type"] = stringify(contentType);
    request.body = serialize(contentType, call);

    Future<http::Response> response = manager.api(request, None());

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    ASSERT_EQ(http::Response::PIPE, response->type);

    ASSERT_TRUE(response->headers.contains("Mesos-Stream-Id"));
    Try<id::UUID> uuid =
      id::UUID::fromString(response->headers.at("Mesos-Stream-Id"));

    CHECK_SOME(uuid);
    streamId = uuid.get();

    Option<http::Pipe::Reader> reader = response->reader;
    ASSERT_SOME(reader);

    responseDecoder.reset(new recordio::Reader<Event>(
        ::recordio::Decoder<Event>(
            lambda::bind(deserialize<Event>, contentType, lambda::_1)),
        reader.get()));

    Future<Result<Event>> event = responseDecoder->read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());

    // Check event type is subscribed and the resource provider id is set.
    ASSERT_EQ(Event::SUBSCRIBED, event->get().type());

    resourceProviderId = event->get().subscribed().provider_id();

    EXPECT_FALSE(resourceProviderId->value().empty());
  }

  // Then, update the publish status with `OK`.
  {
    vector<v1::Resource> resources =
      v1::Resources::fromString("disk:4").get();
    foreach (v1::Resource& resource, resources) {
      resource.mutable_provider_id()->CopyFrom(resourceProviderId.get());
    }

    Future<Nothing> published = manager.publishResources(devolve(resources));

    Future<Result<Event>> event = responseDecoder->read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());
    ASSERT_EQ(Event::PUBLISH_RESOURCES, event->get().type());

    Call call;
    call.set_type(Call::UPDATE_PUBLISH_RESOURCES_STATUS);
    call.mutable_resource_provider_id()->CopyFrom(resourceProviderId.get());

    Call::UpdatePublishResourcesStatus* update =
      call.mutable_update_publish_resources_status();
    update->mutable_uuid()->CopyFrom(event->get().publish_resources().uuid());
    update->set_status(Call::UpdatePublishResourcesStatus::OK);

    http::Request request;
    request.method = "POST";
    request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    request.headers["Accept"] = stringify(contentType);
    request.headers["Content-Type"] = stringify(contentType);
    request.headers["Mesos-Stream-Id"] = stringify(streamId.get());
    request.body = serialize(contentType, call);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        Accepted().status,
        manager.api(request, None()));

    // The manager should satisfy the future.
    AWAIT_READY(published);
  }
}


// This test verifies that the pending future returned by
// `ResourceProviderManager::publishResources()` becomes failed when the manager
// receives an publish status update with a `FAILED` status.
TEST_P(ResourceProviderManagerHttpApiTest, PublishResourcesFailure)
{
  const ContentType contentType = GetParam();

  ResourceProviderManager manager(
      Registrar::create(Owned<Storage>(new InMemoryStorage)).get());

  Option<id::UUID> streamId;
  Option<mesos::v1::ResourceProviderID> resourceProviderId;
  Owned<recordio::Reader<Event>> responseDecoder;

  // First, subscribe to the manager to get the ID.
  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();

    mesos::v1::ResourceProviderInfo* info =
      subscribe->mutable_resource_provider_info();

    info->set_type("org.apache.mesos.rp.test");
    info->set_name("test");

    http::Request request;
    request.method = "POST";
    request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    request.headers["Accept"] = stringify(contentType);
    request.headers["Content-Type"] = stringify(contentType);
    request.body = serialize(contentType, call);

    Future<http::Response> response = manager.api(request, None());

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    ASSERT_EQ(http::Response::PIPE, response->type);

    ASSERT_TRUE(response->headers.contains("Mesos-Stream-Id"));
    Try<id::UUID> uuid =
      id::UUID::fromString(response->headers.at("Mesos-Stream-Id"));

    CHECK_SOME(uuid);
    streamId = uuid.get();

    Option<http::Pipe::Reader> reader = response->reader;
    ASSERT_SOME(reader);

    responseDecoder.reset(new recordio::Reader<Event>(
        ::recordio::Decoder<Event>(
            lambda::bind(deserialize<Event>, contentType, lambda::_1)),
        reader.get()));

    Future<Result<Event>> event = responseDecoder->read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());

    // Check event type is subscribed and the resource provider id is set.
    ASSERT_EQ(Event::SUBSCRIBED, event->get().type());

    resourceProviderId = event->get().subscribed().provider_id();

    EXPECT_FALSE(resourceProviderId->value().empty());
  }

  // Then, update the publish status with `FAILED`.
  {
    vector<v1::Resource> resources =
      v1::Resources::fromString("disk:4").get();
    foreach (v1::Resource& resource, resources) {
      resource.mutable_provider_id()->CopyFrom(resourceProviderId.get());
    }

    Future<Nothing> published = manager.publishResources(devolve(resources));

    Future<Result<Event>> event = responseDecoder->read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());
    ASSERT_EQ(Event::PUBLISH_RESOURCES, event->get().type());

    Call call;
    call.set_type(Call::UPDATE_PUBLISH_RESOURCES_STATUS);
    call.mutable_resource_provider_id()->CopyFrom(resourceProviderId.get());

    Call::UpdatePublishResourcesStatus* update =
      call.mutable_update_publish_resources_status();
    update->mutable_uuid()->CopyFrom(event->get().publish_resources().uuid());
    update->set_status(Call::UpdatePublishResourcesStatus::FAILED);

    http::Request request;
    request.method = "POST";
    request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    request.headers["Accept"] = stringify(contentType);
    request.headers["Content-Type"] = stringify(contentType);
    request.headers["Mesos-Stream-Id"] = stringify(streamId.get());
    request.body = serialize(contentType, call);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        Accepted().status,
        manager.api(request, None()));

    // The manager should fail the future.
    AWAIT_FAILED(published);
  }
}


// This test verifies that the pending future returned by
// `ResourceProviderManager::publishResources()` becomes failed when the
// resource provider is disconnected.
TEST_P(ResourceProviderManagerHttpApiTest, PublishResourcesDisconnected)
{
  const ContentType contentType = GetParam();

  ResourceProviderManager manager(
      Registrar::create(Owned<Storage>(new InMemoryStorage)).get());

  Option<mesos::v1::ResourceProviderID> resourceProviderId;
  Option<http::Pipe::Reader> reader;
  Owned<recordio::Reader<Event>> responseDecoder;

  // First, subscribe to the manager to get the ID.
  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();

    mesos::v1::ResourceProviderInfo* info =
      subscribe->mutable_resource_provider_info();

    info->set_type("org.apache.mesos.rp.test");
    info->set_name("test");

    http::Request request;
    request.method = "POST";
    request.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    request.headers["Accept"] = stringify(contentType);
    request.headers["Content-Type"] = stringify(contentType);
    request.body = serialize(contentType, call);

    Future<http::Response> response = manager.api(request, None());

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    ASSERT_EQ(http::Response::PIPE, response->type);

    reader = response->reader;
    ASSERT_SOME(reader);

    responseDecoder.reset(new recordio::Reader<Event>(
        ::recordio::Decoder<Event>(
            lambda::bind(deserialize<Event>, contentType, lambda::_1)),
        reader.get()));

    Future<Result<Event>> event = responseDecoder->read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());

    // Check event type is subscribed and the resource provider id is set.
    ASSERT_EQ(Event::SUBSCRIBED, event->get().type());

    resourceProviderId = event->get().subscribed().provider_id();

    EXPECT_FALSE(resourceProviderId->value().empty());
  }

  // Then, close the connection after receiving a publish resources event.
  {
    vector<v1::Resource> resources =
      v1::Resources::fromString("disk:4").get();
    foreach (v1::Resource& resource, resources) {
      resource.mutable_provider_id()->CopyFrom(resourceProviderId.get());
    }

    Future<Nothing> published = manager.publishResources(devolve(resources));

    Future<Result<Event>> event = responseDecoder->read();
    AWAIT_READY(event);
    ASSERT_SOME(event.get());
    ASSERT_EQ(Event::PUBLISH_RESOURCES, event->get().type());

    reader->close();

    // The manager should fail the future.
    AWAIT_FAILED(published);
  }
}


// This test starts an agent and connects directly with its resource
// provider endpoint.
TEST_P(ResourceProviderManagerHttpApiTest, AgentEndpoint)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // For the agent's resource provider manager to start,
  // the agent needs to have been assigned an agent ID.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

  AWAIT_READY(slaveRegisteredMessage);

  // Wait for recovery to be complete.
  Clock::pause();
  Clock::settle();

  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();

  mesos::v1::ResourceProviderInfo* info =
    subscribe->mutable_resource_provider_info();

  info->set_type("org.apache.mesos.rp.test");
  info->set_name("test");

  const ContentType contentType = GetParam();

  http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  Future<http::Response> response = http::streaming::post(
      agent.get()->pid,
      "api/v1/resource_provider",
      headers,
      serialize(contentType, call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ("chunked", "Transfer-Encoding", response);
  ASSERT_EQ(http::Response::PIPE, response->type);

  Option<http::Pipe::Reader> reader = response->reader;
  ASSERT_SOME(reader);

  recordio::Reader<Event> responseDecoder(
      ::recordio::Decoder<Event>(
          lambda::bind(deserialize<Event>, contentType, lambda::_1)),
      reader.get());

  Future<Result<Event>> event = responseDecoder.read();
  AWAIT_READY(event);
  ASSERT_SOME(event.get());

  // Check event type is subscribed and the resource provider id is set.
  EXPECT_EQ(Event::SUBSCRIBED, event->get().type());
  EXPECT_FALSE(event->get().subscribed().provider_id().value().empty());
}


class ResourceProviderRegistrarTest : public tests::MesosTest {};


// Test that the generic resource provider registrar works as expected.
//
// TODO(bbannier): Enable this test on Windows once MESOS-5932 is resolved.
#ifndef __WINDOWS__
TEST_F_TEMP_DISABLED_ON_WINDOWS(ResourceProviderRegistrarTest, GenericRegistrar)
{
  mesos::resource_provider::registry::ResourceProvider resourceProvider;
  resourceProvider.mutable_id()->set_value("foo");
  resourceProvider.set_name("bar");
  resourceProvider.set_type("org.apache.mesos.rp.test");

  // Perform operations on the resource provider. We use
  // persistent storage so we can recover the state below.
  {
    Owned<mesos::state::Storage> storage(
        new mesos::state::LevelDBStorage(sandbox.get()));
    Try<Owned<Registrar>> registrar = Registrar::create(std::move(storage));

    ASSERT_SOME_NE(Owned<Registrar>(nullptr), registrar);

    Future<mesos::resource_provider::registry::Registry> recover =
      registrar.get()->recover();
    AWAIT_READY(recover);
    EXPECT_TRUE(recover->removed_resource_providers().empty());

    Future<bool> admitResourceProvider =
      registrar.get()->apply(Owned<Registrar::Operation>(
          new AdmitResourceProvider(resourceProvider)));
    AWAIT_READY(admitResourceProvider);
    EXPECT_TRUE(admitResourceProvider.get());

    // A resource provider cannot resubscribe with changed type or name.
    mesos::resource_provider::registry::ResourceProvider resourceProvider_ =
      resourceProvider;
    resourceProvider_.set_type("org.apache.mesos.rp.test2");

    admitResourceProvider =
      registrar.get()->apply(Owned<Registrar::Operation>(
          new AdmitResourceProvider(resourceProvider_)));
    AWAIT_READY(admitResourceProvider);
    EXPECT_FALSE(admitResourceProvider.get());

    Future<bool> removeResourceProvider =
      registrar.get()->apply(Owned<Registrar::Operation>(
          new RemoveResourceProvider(resourceProvider.id())));
    AWAIT_READY(removeResourceProvider);
    EXPECT_TRUE(removeResourceProvider.get());

    // A removed resource provider cannot be admitted again.
    admitResourceProvider =
      registrar.get()->apply(Owned<Registrar::Operation>(
          new AdmitResourceProvider(resourceProvider)));
    AWAIT_READY(admitResourceProvider);
    EXPECT_FALSE(admitResourceProvider.get());
  }

  // Recover and validate the previous registry state.
  {
    Owned<mesos::state::Storage> storage(
        new mesos::state::LevelDBStorage(sandbox.get()));
    Try<Owned<Registrar>> registrar = Registrar::create(std::move(storage));

    ASSERT_SOME_NE(Owned<Registrar>(nullptr), registrar);

    Future<mesos::resource_provider::registry::Registry> recover =
      registrar.get()->recover();
    AWAIT_READY(recover);

    EXPECT_TRUE(recover->resource_providers().empty());
    ASSERT_EQ(1, recover->removed_resource_providers_size());

    const mesos::resource_provider::registry::ResourceProvider&
      resourceProvider_ = recover->removed_resource_providers(0);

    EXPECT_EQ(resourceProvider, resourceProvider_);
  }
}
#endif // __WINDOWS__


// Test that the master resource provider registrar works as expected.
//
// TODO(bbannier): Enable this test on Windows once MESOS-5932 is resolved.
#ifndef __WINDOWS__
TEST_F_TEMP_DISABLED_ON_WINDOWS(ResourceProviderRegistrarTest, MasterRegistrar)
{
  mesos::resource_provider::registry::ResourceProvider resourceProvider;
  resourceProvider.mutable_id()->set_value("foo");
  resourceProvider.set_name("bar");
  resourceProvider.set_type("org.apache.mesos.rp.test");

  // Perform operations on the resource provider. We use
  // persistent storage so we can recover the state below.
  {
    mesos::state::LevelDBStorage storage(sandbox.get());
    State state(&storage);
    master::Registrar masterRegistrar(CreateMasterFlags(), &state);

    const MasterInfo masterInfo = protobuf::createMasterInfo({});

    Future<Registry> registry = masterRegistrar.recover(masterInfo);
    AWAIT_READY(registry);

    Try<Owned<Registrar>> registrar = Registrar::create(
        &masterRegistrar,
        registry->resource_provider_registry());

    ASSERT_SOME_NE(Owned<Registrar>(nullptr), registrar);

    Future<bool> admitResourceProvider =
      registrar.get()->apply(Owned<Registrar::Operation>(
            new AdmitResourceProvider(resourceProvider)));
    AWAIT_READY(admitResourceProvider);
    EXPECT_TRUE(admitResourceProvider.get());

    // A resource provider cannot resubscribe with changed type or name.
    mesos::resource_provider::registry::ResourceProvider resourceProvider_ =
      resourceProvider;
    resourceProvider_.set_type("org.apache.mesos.rp.test2");

    admitResourceProvider =
      registrar.get()->apply(Owned<Registrar::Operation>(
          new AdmitResourceProvider(resourceProvider_)));
    AWAIT_READY(admitResourceProvider);
    EXPECT_FALSE(admitResourceProvider.get());

    Future<bool> removeResourceProvider =
      registrar.get()->apply(Owned<Registrar::Operation>(
            new RemoveResourceProvider(resourceProvider.id())));
    AWAIT_READY(removeResourceProvider);
    EXPECT_TRUE(removeResourceProvider.get());

    // A removed resource provider cannot be admitted again.
    admitResourceProvider =
      registrar.get()->apply(Owned<Registrar::Operation>(
            new AdmitResourceProvider(resourceProvider)));
    AWAIT_READY(admitResourceProvider);
    EXPECT_FALSE(admitResourceProvider.get());
  }

  // Recover and validate the previous registry state.
  {
    mesos::state::LevelDBStorage storage(sandbox.get());
    State state(&storage);
    master::Registrar masterRegistrar(CreateMasterFlags(), &state);

    const MasterInfo masterInfo = protobuf::createMasterInfo({});

    Future<Registry> registry = masterRegistrar.recover(masterInfo);
    AWAIT_READY(registry);

    Try<Owned<Registrar>> registrar = Registrar::create(
        &masterRegistrar,
        registry->resource_provider_registry());

    ASSERT_SOME_NE(Owned<Registrar>(nullptr), registrar);

    Future<mesos::resource_provider::registry::Registry> recover =
      registrar.get()->recover();
    AWAIT_READY(recover);

    EXPECT_TRUE(recover->resource_providers().empty());
    ASSERT_EQ(1, recover->removed_resource_providers_size());

    const mesos::resource_provider::registry::ResourceProvider&
      resourceProvider_ = recover->removed_resource_providers(0);

    EXPECT_EQ(resourceProvider, resourceProvider_);
  }
}
#endif // __WINDOWS__


// Test that resource provider resources are offered to frameworks,
// frameworks can accept the offer with an operation that has a resource
// provider convert resources and that the converted resources are
// offered to frameworks as well.
TEST_P(ResourceProviderManagerHttpApiTest, ConvertResources)
{
  // Start master and agent.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  // Pause the clock and control it manually in order to
  // control the timing of the registration. A registration timeout
  // would trigger multiple registration attempts. As a result, multiple
  // 'UpdateSlaveMessage' would be sent, which we need to avoid to
  // ensure that the second 'UpdateSlaveMessage' is a result of the
  // resource provider registration.
  Clock::pause();

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(agent);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

  v1::Resource disk = v1::createDiskResource(
      "200", "*", None(), None(), v1::createDiskSourceRaw(None(), "profile"));

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Clock::resume();

  mesos::v1::ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.rp.test");
  resourceProviderInfo.set_name("test");

  v1::MockResourceProvider resourceProvider(
      resourceProviderInfo, Some(v1::Resources(disk)));

  // Start and register a resource provider.
  Owned<EndpointDetector> endpointDetector(
      resource_provider::createEndpointDetector(agent.get()->pid));

  const ContentType contentType = GetParam();

  resourceProvider.start(endpointDetector, contentType);

  // Wait until the agent's resources have been updated to include the
  // resource provider resources.
  AWAIT_READY(updateSlaveMessage);

  // Start and register a framework.
  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role");

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers1;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Resource provider resources will be offered to the framework.
  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->offers().empty());

  const v1::Offer& offer1 = offers1->offers(0);

  v1::Resources resources =
    v1::Resources(offer1.resources()).filter([](const v1::Resource& resource) {
      return resource.has_provider_id();
    });

  ASSERT_FALSE(resources.empty());

  v1::Resource reserved = *(resources.begin());
  reserved.add_reservations()->CopyFrom(
      v1::createDynamicReservationInfo(
          frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  Future<v1::scheduler::Event::Offers> offers2;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer1,
          {v1::RESERVE(reserved),
           v1::CREATE_DISK(reserved, v1::Resource::DiskInfo::Source::MOUNT)}));

  // The converted resource should be offered to the framework.
  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->offers().empty());

  const v1::Offer& offer2 = offers2->offers(0);

  Option<v1::Resource> mountDisk;
  foreach (const v1::Resource& resource, offer2.resources()) {
    if (resource.has_provider_id()) {
      mountDisk = resource;
    }
  }

  ASSERT_SOME(mountDisk);
  EXPECT_EQ(
      v1::Resource::DiskInfo::Source::MOUNT, mountDisk->disk().source().type());
  EXPECT_FALSE(mountDisk->reservations().empty());
}


// Test that resource provider can resubscribe with an agent after
// a resource provider failover as well as an agent failover.
//
// TODO(bbannier): This test is currently disabled on Windows as the resource
// provider manager uses a LevelDB store which is at the moment not supported on
// Windows (see MESOS-5932) and we use an in-memory store. The in-memory store
// does not survive agent restarts so that resubscription attempts are rejected
// (resource provider is unknown).
TEST_P_TEMP_DISABLED_ON_WINDOWS(
    ResourceProviderManagerHttpApiTest, ResubscribeResourceProvider)
{
  Clock::pause();

  // Start master and agent.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(agent);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  mesos::v1::ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.rp.test");
  resourceProviderInfo.set_name("test");

  v1::Resource disk = v1::createDiskResource(
      "200", "*", None(), None(), v1::createDiskSourceRaw());

  Owned<v1::MockResourceProvider> resourceProvider(
      new v1::MockResourceProvider(resourceProviderInfo, v1::Resources(disk)));

  // Start and register a resource provider.
  Owned<EndpointDetector> endpointDetector(
      resource_provider::createEndpointDetector(agent.get()->pid));

  const ContentType contentType = GetParam();

  resourceProvider->start(endpointDetector, contentType);

  // Wait until the agent's resources have been updated to include the
  // resource provider resources. At this point the resource provider
  // will have an ID assigned by the agent.
  AWAIT_READY(updateSlaveMessage);

  ASSERT_TRUE(resourceProvider->info.has_id());

  resourceProviderInfo = resourceProvider->info;

  // Resource provider failover by opening a new connection.
  // The assigned resource provider ID will be used to resubscribe.
  resourceProvider.reset(
      new v1::MockResourceProvider(resourceProviderInfo, v1::Resources(disk)));

  Future<Event::Subscribed> subscribed1;
  EXPECT_CALL(*resourceProvider, subscribed(_))
    .WillOnce(FutureArg<0>(&subscribed1));

  resourceProvider->start(endpointDetector, contentType);

  AWAIT_READY(subscribed1);
  EXPECT_EQ(resourceProviderInfo.id(), subscribed1->provider_id());

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  // We terminate the resource provider once we have confirmed that it
  // got disconnected. This avoids it to in turn resubscribe racing
  // with the newly created resource provider.
  Future<Nothing> disconnected;
  EXPECT_CALL(*resourceProvider, disconnected())
    .WillOnce(DoAll(
        Invoke([&resourceProvider]() { resourceProvider.reset(); }),
        FutureSatisfy(&disconnected)))
    .WillRepeatedly(Return()); // Ignore spurious calls concurrent with `reset`.

  // The agent failover.
  agent->reset();

  AWAIT_READY(disconnected);

  agent = StartSlave(detector.get(), slaveFlags);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();

  AWAIT_READY(__recover);

  endpointDetector =
    resource_provider::createEndpointDetector(agent.get()->pid);

  resourceProvider.reset(new v1::MockResourceProvider(
      resourceProviderInfo,
      Some(v1::Resources(disk))));

  Future<Event::Subscribed> subscribed2;
  EXPECT_CALL(*resourceProvider, subscribed(_))
    .WillOnce(FutureArg<0>(&subscribed2));

  resourceProvider->start(endpointDetector, contentType);

  AWAIT_READY(subscribed2);
  EXPECT_EQ(resourceProviderInfo.id(), subscribed2->provider_id());
}


// Test that when a resource provider attempts to resubscribe with an
// unknown ID it is not admitted but disconnected.
TEST_P(ResourceProviderManagerHttpApiTest, ResubscribeUnknownID)
{
  Clock::pause();

  // Start master and agent.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  // For the agent's resource provider manager to start,
  // the agent needs to have been assigned an agent ID.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(agent);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();

  AWAIT_READY(slaveRegisteredMessage);

  mesos::v1::ResourceProviderID resourceProviderId;
  resourceProviderId.set_value(id::UUID::random().toString());

  mesos::v1::ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.mutable_id()->CopyFrom(resourceProviderId);
  resourceProviderInfo.set_type("org.apache.mesos.rp.test");
  resourceProviderInfo.set_name("test");

  Owned<v1::MockResourceProvider> resourceProvider(
      new v1::MockResourceProvider(resourceProviderInfo));

  // We explicitly reset the resource provider after the expected
  // disconnect to prevent it from resubscribing indefinitely.
  Future<Nothing> disconnected;
  EXPECT_CALL(*resourceProvider, disconnected())
    .WillOnce(DoAll(
        Invoke([&resourceProvider]() { resourceProvider.reset(); }),
        FutureSatisfy(&disconnected)));

  // Start and register a resource provider.
  Owned<EndpointDetector> endpointDetector(
      resource_provider::createEndpointDetector(agent.get()->pid));

  const ContentType contentType = GetParam();

  resourceProvider->start(endpointDetector, contentType);

  AWAIT_READY(disconnected);
}


// This test verifies that a disconnected resource provider will
// result in an `UpdateSlaveMessage` to be sent to the master and the
// total resources of the disconnected resource provider will be
// reduced to empty.
TEST_P(ResourceProviderManagerHttpApiTest, ResourceProviderDisconnect)
{
  Clock::pause();

  // Start master and agent.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(agent);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();

  AWAIT_READY(updateSlaveMessage);

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  mesos::v1::ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.rp.test");
  resourceProviderInfo.set_name("test");

  v1::Resource disk = v1::createDiskResource(
      "200", "*", None(), None(), v1::createDiskSourceRaw());

  Owned<v1::MockResourceProvider> resourceProvider(
      new v1::MockResourceProvider(
          resourceProviderInfo,
          v1::Resources(disk)));

  // Start and register a resource provider.
  Owned<EndpointDetector> endpointDetector(
      resource_provider::createEndpointDetector(agent.get()->pid));

  const ContentType contentType = GetParam();

  resourceProvider->start(endpointDetector, contentType);

  {
    // Wait until the agent's resources have been updated to include
    // the resource provider resources. At this point the resource
    // provider will have an ID assigned by the agent.
    AWAIT_READY(updateSlaveMessage);

    ASSERT_TRUE(resourceProvider->info.has_id());
    disk.mutable_provider_id()->CopyFrom(resourceProvider->info.id());

    const Resources& totalResources =
      updateSlaveMessage->resource_providers().providers(0).total_resources();

    EXPECT_TRUE(totalResources.contains(devolve(disk)));
  }

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  // Simulate a resource provider disconnection.
  resourceProvider.reset();

  {
    AWAIT_READY(updateSlaveMessage);
    ASSERT_TRUE(updateSlaveMessage->has_resource_providers());
    ASSERT_EQ(1, updateSlaveMessage->resource_providers().providers_size());

    const Resources& totalResources =
      updateSlaveMessage->resource_providers().providers(0).total_resources();

    EXPECT_FALSE(totalResources.contains(devolve(disk)));
  }
}


// This test verifies that if a second resource provider subscribes
// with the ID of an already connected resource provider, the first
// instance gets disconnected and the second subscription is handled
// as a resubscription.
TEST_F(ResourceProviderManagerHttpApiTest, ResourceProviderSubscribeDisconnect)
{
  Clock::pause();

  // Start master and agent.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(agent);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();
  AWAIT_READY(updateSlaveMessage);

  mesos::v1::ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.rp.test");
  resourceProviderInfo.set_name("test");

  Owned<v1::MockResourceProvider> resourceProvider1(
      new v1::MockResourceProvider(resourceProviderInfo));

  // Start and register a resource provider.
  Owned<EndpointDetector> endpointDetector(
      resource_provider::createEndpointDetector(agent.get()->pid));

  Future<Event::Subscribed> subscribed1;
  EXPECT_CALL(*resourceProvider1, subscribed(_))
    .WillOnce(FutureArg<0>(&subscribed1));

  resourceProvider1->start(endpointDetector, ContentType::PROTOBUF);

  AWAIT_READY(subscribed1);

  resourceProviderInfo.mutable_id()->CopyFrom(subscribed1->provider_id());

  // Subscribing a second resource provider with the same ID will
  // disconnect the first instance and handle the subscription by the
  // second resource provider as a resubscription.
  Owned<v1::MockResourceProvider> resourceProvider2(
      new v1::MockResourceProvider(resourceProviderInfo));

  // We terminate the first resource provider once we have confirmed
  // that it got disconnected. This avoids it to in turn resubscribe
  // racing with the other resource provider.
  Future<Nothing> disconnected1;
  EXPECT_CALL(*resourceProvider1, disconnected())
    .WillOnce(DoAll(
        Invoke([&resourceProvider1]() { resourceProvider1.reset(); }),
        FutureSatisfy(&disconnected1)))
    .WillRepeatedly(Return()); // Ignore spurious calls concurrent with `reset`.

  Future<Event::Subscribed> subscribed2;
  EXPECT_CALL(*resourceProvider2, subscribed(_))
    .WillOnce(FutureArg<0>(&subscribed2));

  resourceProvider2->start(endpointDetector, ContentType::PROTOBUF);

  AWAIT_READY(disconnected1);
  AWAIT_READY(subscribed2);
}


TEST_F(ResourceProviderManagerHttpApiTest, Metrics)
{
  Clock::pause();

  // Start master and agent.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(agent);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();

  AWAIT_READY(updateSlaveMessage);

  mesos::v1::ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.rp.test");
  resourceProviderInfo.set_name("test");

  Owned<v1::MockResourceProvider> resourceProvider(
      new v1::MockResourceProvider(resourceProviderInfo));

  // Start and register a resource provider.
  Owned<EndpointDetector> endpointDetector(
      resource_provider::createEndpointDetector(agent.get()->pid));

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*resourceProvider, subscribed(_))
    .WillOnce(FutureArg<0>(&subscribed));

  resourceProvider->start(endpointDetector, ContentType::PROTOBUF);

  AWAIT_READY(subscribed);

  const JSON::Object snapshot = Metrics();

  EXPECT_EQ(1, snapshot.values.at("resource_provider_manager/subscribed"));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
