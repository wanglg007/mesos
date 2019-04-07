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

#include "master/master.hpp"

#include "common/protobuf_utils.hpp"

namespace mesos {
namespace internal {
namespace master {

Framework::Framework(
    Master* const master,
    const Flags& masterFlags,
    const FrameworkInfo& info,
    const process::UPID& _pid,
    const process::Time& time)
  : Framework(master, masterFlags, info, ACTIVE, time)
{
  pid = _pid;
}


Framework::Framework(
    Master* const master,
    const Flags& masterFlags,
    const FrameworkInfo& info,
    const HttpConnection& _http,
    const process::Time& time)
  : Framework(master, masterFlags, info, ACTIVE, time)
{
  http = _http;
}


Framework::Framework(
    Master* const master,
    const Flags& masterFlags,
    const FrameworkInfo& info)
  : Framework(master, masterFlags, info, RECOVERED, process::Time())
{}


Framework::Framework(
    Master* const _master,
    const Flags& masterFlags,
    const FrameworkInfo& _info,
    State state,
    const process::Time& time)
  : master(_master),
    info(_info),
    roles(protobuf::framework::getRoles(_info)),
    capabilities(_info.capabilities()),
    state(state),
    registeredTime(time),
    reregisteredTime(time),
    completedTasks(masterFlags.max_completed_tasks_per_framework),
    unreachableTasks(masterFlags.max_unreachable_tasks_per_framework),
    metrics(_info)
{
  CHECK(_info.has_id());

  setFrameworkState(state);

  foreach (const std::string& role, roles) {
    // NOTE: It's possible that we're already being tracked under the role
    // because a framework can unsubscribe from a role while it still has
    // resources allocated to the role.
    if (!isTrackedUnderRole(role)) {
      trackUnderRole(role);
    }
  }
}


Framework::~Framework()
{
  if (http.isSome()) {
    closeHttpConnection();
  }
}


Task* Framework::getTask(const TaskID& taskId)
{
  if (tasks.count(taskId) > 0) {
    return tasks[taskId];
  }

  return nullptr;
}


void Framework::addTask(Task* task)
{
  CHECK(!tasks.contains(task->task_id()))
    << "Duplicate task " << task->task_id()
    << " of framework " << task->framework_id();

  // Verify that Resource.AllocationInfo is set,
  // this should be guaranteed by the master.
  foreach (const Resource& resource, task->resources()) {
    CHECK(resource.has_allocation_info());
  }

  tasks[task->task_id()] = task;

  // Unreachable tasks should be added via `addUnreachableTask`.
  CHECK(task->state() != TASK_UNREACHABLE)
    << "Task '" << task->task_id() << "' of framework " << id()
    << " added in TASK_UNREACHABLE state";

  // Since we track terminal but unacknowledged tasks within
  // `tasks` rather than `completedTasks`, we need to handle
  // them here: don't count them as consuming resources.
  //
  // TODO(bmahler): Users currently get confused because
  // terminal tasks can show up as "active" tasks in the UI and
  // endpoints. Ideally, we show the terminal unacknowledged
  // tasks as "completed" as well.
  if (!protobuf::isTerminalState(task->state())) {
    // Note that we explicitly convert from protobuf to `Resources` once
    // and then use the result for calculations to avoid performance penalty
    // for multiple conversions and validations implied by `+=` with protobuf
    // arguments.
    // Conversion is safe, as resources have already passed validation.
    const Resources resources = task->resources();
    totalUsedResources += resources;
    usedResources[task->slave_id()] += resources;

    // It's possible that we're not tracking the task's role for
    // this framework if the role is absent from the framework's
    // set of roles. In this case, we track the role's allocation
    // for this framework.
    CHECK(!task->resources().empty());
    const std::string& role =
      task->resources().begin()->allocation_info().role();

    if (!isTrackedUnderRole(role)) {
      trackUnderRole(role);
    }
  }

  metrics.incrementTaskState(task->state());

  if (!master->subscribers.subscribed.empty()) {
    master->subscribers.send(
        protobuf::master::event::createTaskAdded(*task),
        info);
  }
}


void Framework::recoverResources(Task* task)
{
  CHECK(tasks.contains(task->task_id()))
    << "Unknown task " << task->task_id()
    << " of framework " << task->framework_id();

  totalUsedResources -= task->resources();
  usedResources[task->slave_id()] -= task->resources();
  if (usedResources[task->slave_id()].empty()) {
    usedResources.erase(task->slave_id());
  }

  // If we are no longer subscribed to the role to which these resources are
  // being returned to, and we have no more resources allocated to us for that
  // role, stop tracking the framework under the role.
  CHECK(!task->resources().empty());
  const std::string& role =
    task->resources().begin()->allocation_info().role();

  auto allocatedToRole = [&role](const Resource& resource) {
    return resource.allocation_info().role() == role;
  };

  if (roles.count(role) == 0 &&
      totalUsedResources.filter(allocatedToRole).empty()) {
    CHECK(totalOfferedResources.filter(allocatedToRole).empty());
    untrackUnderRole(role);
  }
}


void Framework::addCompletedTask(Task&& task)
{
  // TODO(neilc): We currently allow frameworks to reuse the task
  // IDs of completed tasks (although this is discouraged). This
  // means that there might be multiple completed tasks with the
  // same task ID. We should consider rejecting attempts to reuse
  // task IDs (MESOS-6779).
  completedTasks.push_back(process::Owned<Task>(new Task(std::move(task))));
}


void Framework::addUnreachableTask(const Task& task)
{
  // TODO(adam-mesos): Check if unreachable task already exists.
  unreachableTasks.set(task.task_id(), process::Owned<Task>(new Task(task)));
}


void Framework::removeTask(Task* task, bool unreachable)
{
  CHECK(tasks.contains(task->task_id()))
    << "Unknown task " << task->task_id()
    << " of framework " << task->framework_id();

  // The invariant here is that the master will have already called
  // `recoverResources()` prior to removing terminal or unreachable tasks.
  if (!protobuf::isTerminalState(task->state()) &&
      task->state() != TASK_UNREACHABLE) {
    recoverResources(task);
  }

  if (unreachable) {
    addUnreachableTask(*task);
  } else {
    CHECK(task->state() != TASK_UNREACHABLE);

    // TODO(bmahler): This moves a potentially non-terminal task into
    // the completed list!
    addCompletedTask(Task(*task));
  }

  tasks.erase(task->task_id());
}


void Framework::addOffer(Offer* offer)
{
  CHECK(!offers.contains(offer)) << "Duplicate offer " << offer->id();
  offers.insert(offer);
  totalOfferedResources += offer->resources();
  offeredResources[offer->slave_id()] += offer->resources();
}


void Framework::removeOffer(Offer* offer)
{
  CHECK(offers.find(offer) != offers.end())
    << "Unknown offer " << offer->id();

  totalOfferedResources -= offer->resources();
  offeredResources[offer->slave_id()] -= offer->resources();
  if (offeredResources[offer->slave_id()].empty()) {
    offeredResources.erase(offer->slave_id());
  }

  offers.erase(offer);
}


void Framework::addInverseOffer(InverseOffer* inverseOffer)
{
  CHECK(!inverseOffers.contains(inverseOffer))
    << "Duplicate inverse offer " << inverseOffer->id();
  inverseOffers.insert(inverseOffer);
}


void Framework::removeInverseOffer(InverseOffer* inverseOffer)
{
  CHECK(inverseOffers.contains(inverseOffer))
    << "Unknown inverse offer " << inverseOffer->id();

  inverseOffers.erase(inverseOffer);
}


bool Framework::hasExecutor(
    const SlaveID& slaveId,
    const ExecutorID& executorId)
{
  return executors.contains(slaveId) &&
    executors[slaveId].contains(executorId);
}


void Framework::addExecutor(
    const SlaveID& slaveId,
    const ExecutorInfo& executorInfo)
{
  CHECK(!hasExecutor(slaveId, executorInfo.executor_id()))
    << "Duplicate executor '" << executorInfo.executor_id()
    << "' on agent " << slaveId;

  // Verify that Resource.AllocationInfo is set,
  // this should be guaranteed by the master.
  foreach (const Resource& resource, executorInfo.resources()) {
    CHECK(resource.has_allocation_info());
  }

  executors[slaveId][executorInfo.executor_id()] = executorInfo;
  totalUsedResources += executorInfo.resources();
  usedResources[slaveId] += executorInfo.resources();

  // It's possible that we're not tracking the task's role for
  // this framework if the role is absent from the framework's
  // set of roles. In this case, we track the role's allocation
  // for this framework.
  if (!executorInfo.resources().empty()) {
    const std::string& role =
      executorInfo.resources().begin()->allocation_info().role();

    if (!isTrackedUnderRole(role)) {
      trackUnderRole(role);
    }
  }
}


void Framework::removeExecutor(
    const SlaveID& slaveId,
    const ExecutorID& executorId)
{
  CHECK(hasExecutor(slaveId, executorId))
    << "Unknown executor '" << executorId
    << "' of framework " << id()
    << " of agent " << slaveId;

  const ExecutorInfo& executorInfo = executors[slaveId][executorId];

  totalUsedResources -= executorInfo.resources();
  usedResources[slaveId] -= executorInfo.resources();
  if (usedResources[slaveId].empty()) {
    usedResources.erase(slaveId);
  }

  // If we are no longer subscribed to the role to which these resources are
  // being returned to, and we have no more resources allocated to us for that
  // role, stop tracking the framework under the role.
  if (!executorInfo.resources().empty()) {
    const std::string& role =
      executorInfo.resources().begin()->allocation_info().role();

    auto allocatedToRole = [&role](const Resource& resource) {
      return resource.allocation_info().role() == role;
    };

    if (roles.count(role) == 0 &&
        totalUsedResources.filter(allocatedToRole).empty()) {
      CHECK(totalOfferedResources.filter(allocatedToRole).empty());
      untrackUnderRole(role);
    }
  }

  executors[slaveId].erase(executorId);
  if (executors[slaveId].empty()) {
    executors.erase(slaveId);
  }
}


void Framework::addOperation(Operation* operation)
{
  CHECK(operation->has_framework_id());

  const FrameworkID& frameworkId = operation->framework_id();

  const UUID& uuid = operation->uuid();

  CHECK(!operations.contains(uuid))
    << "Duplicate operation '" << operation->info().id()
    << "' (uuid: " << uuid << ") "
    << "of framework " << frameworkId;

  operations.put(uuid, operation);

  if (operation->info().has_id()) {
    operationUUIDs.put(operation->info().id(), uuid);
  }

  if (!protobuf::isSpeculativeOperation(operation->info()) &&
      !protobuf::isTerminalState(operation->latest_status().state())) {
    Try<Resources> consumed =
      protobuf::getConsumedResources(operation->info());
    CHECK_SOME(consumed);

    CHECK(operation->has_slave_id())
      << "External resource provider is not supported yet";

    const SlaveID& slaveId = operation->slave_id();

    totalUsedResources += consumed.get();
    usedResources[slaveId] += consumed.get();

    // It's possible that we're not tracking the role from the
    // resources in the operation for this framework if the role is
    // absent from the framework's set of roles. In this case, we
    // track the role's allocation for this framework.
    foreachkey (const std::string& role, consumed->allocations()) {
      if (!isTrackedUnderRole(role)) {
        trackUnderRole(role);
      }
    }
  }
}


Option<Operation*> Framework::getOperation(const OperationID& id)
{
  Option<UUID> uuid = operationUUIDs.get(id);

  if (uuid.isNone()) {
    return None();
  }

  Option<Operation*> operation = operations.get(uuid.get());

  CHECK_SOME(operation);

  return operation;
}


void Framework::recoverResources(Operation* operation)
{
  CHECK(operation->has_slave_id())
    << "External resource provider is not supported yet";

  const SlaveID& slaveId = operation->slave_id();

  if (protobuf::isSpeculativeOperation(operation->info())) {
    return;
  }

  Try<Resources> consumed = protobuf::getConsumedResources(operation->info());
  CHECK_SOME(consumed);

  CHECK(totalUsedResources.contains(consumed.get()))
    << "Tried to recover resources " << consumed.get()
    << " which do not seem used";

  CHECK(usedResources[slaveId].contains(consumed.get()))
    << "Tried to recover resources " << consumed.get() << " of agent "
    << slaveId << " which do not seem used";

  totalUsedResources -= consumed.get();
  usedResources[slaveId] -= consumed.get();
  if (usedResources[slaveId].empty()) {
    usedResources.erase(slaveId);
  }

  // If we are no longer subscribed to the role to which these
  // resources are being returned to, and we have no more resources
  // allocated to us for that role, stop tracking the framework
  // under the role.
  foreachkey (const std::string& role, consumed->allocations()) {
    auto allocatedToRole = [&role](const Resource& resource) {
      return resource.allocation_info().role() == role;
    };

    if (roles.count(role) == 0 &&
        totalUsedResources.filter(allocatedToRole).empty()) {
      CHECK(totalOfferedResources.filter(allocatedToRole).empty());
      untrackUnderRole(role);
    }
  }
}


void Framework::removeOperation(Operation* operation)
{
  const UUID& uuid = operation->uuid();

  CHECK(operations.contains(uuid))
    << "Unknown operation '" << operation->info().id()
    << "' (uuid: " << uuid << ") "
    << "of framework " << operation->framework_id();

  if (!protobuf::isSpeculativeOperation(operation->info()) &&
      !protobuf::isTerminalState(operation->latest_status().state())) {
    recoverResources(operation);
  }

  if (operation->info().has_id()) {
    operationUUIDs.erase(operation->info().id());
  }

  operations.erase(uuid);
}


void Framework::update(const FrameworkInfo& newInfo)
{
  // We only merge 'info' from the same framework 'id'.
  CHECK_EQ(info.id(), newInfo.id());

  // Save the old list of roles for later.
  std::set<std::string> oldRoles = roles;

  // TODO(jmlvanre): Merge other fields as per design doc in
  // MESOS-703.

  info.clear_role();
  info.clear_roles();

  if (newInfo.has_role()) {
    info.set_role(newInfo.role());
  }

  if (newInfo.roles_size() > 0) {
    info.mutable_roles()->CopyFrom(newInfo.roles());
  }

  roles = protobuf::framework::getRoles(newInfo);

  if (newInfo.user() != info.user()) {
    LOG(WARNING) << "Cannot update FrameworkInfo.user to '" << newInfo.user()
                 << "' for framework " << id() << ". Check MESOS-703";
  }

  info.set_name(newInfo.name());

  if (newInfo.has_failover_timeout()) {
    info.set_failover_timeout(newInfo.failover_timeout());
  } else {
    info.clear_failover_timeout();
  }

  if (newInfo.checkpoint() != info.checkpoint()) {
    LOG(WARNING) << "Cannot update FrameworkInfo.checkpoint to '"
                 << stringify(newInfo.checkpoint()) << "' for framework "
                 << id() << ". Check MESOS-703";
  }

  if (newInfo.has_hostname()) {
    info.set_hostname(newInfo.hostname());
  } else {
    info.clear_hostname();
  }

  if (newInfo.principal() != info.principal()) {
    LOG(WARNING) << "Cannot update FrameworkInfo.principal to '"
                 << newInfo.principal() << "' for framework " << id()
                 << ". Check MESOS-703";
  }

  if (newInfo.has_webui_url()) {
    info.set_webui_url(newInfo.webui_url());
  } else {
    info.clear_webui_url();
  }

  if (newInfo.capabilities_size() > 0) {
    info.mutable_capabilities()->CopyFrom(newInfo.capabilities());
  } else {
    info.clear_capabilities();
  }
  capabilities = protobuf::framework::Capabilities(info.capabilities());

  if (newInfo.has_labels()) {
    info.mutable_labels()->CopyFrom(newInfo.labels());
  } else {
    info.clear_labels();
  }

  const std::set<std::string>& newRoles = roles;

  const std::set<std::string> removedRoles = [&]() {
    std::set<std::string> result = oldRoles;
    foreach (const std::string& role, newRoles) {
      result.erase(role);
    }
    return result;
  }();

  foreach (const std::string& role, removedRoles) {
    auto allocatedToRole = [&role](const Resource& resource) {
      return resource.allocation_info().role() == role;
    };

    // Stop tracking the framework under this role if there are
    // no longer any resources allocated to it.
    if (totalUsedResources.filter(allocatedToRole).empty()) {
      CHECK(totalOfferedResources.filter(allocatedToRole).empty());
      untrackUnderRole(role);
    }
  }

  const std::set<std::string> addedRoles = [&]() {
    std::set<std::string> result = newRoles;
    foreach (const std::string& role, oldRoles) {
      result.erase(role);
    }
    return result;
  }();

  foreach (const std::string& role, addedRoles) {
    // NOTE: It's possible that we're already tracking this framework
    // under the role because a framework can unsubscribe from a role
    // while it still has resources allocated to the role.
    if (!isTrackedUnderRole(role)) {
      trackUnderRole(role);
    }
  }
}


void Framework::updateConnection(const process::UPID& newPid)
{
  // Cleanup the HTTP connnection if this is a downgrade from HTTP
  // to PID. Note that the connection may already be closed.
  if (http.isSome()) {
    closeHttpConnection();
  }

  // TODO(benh): unlink(oldPid);
  pid = newPid;
}


void Framework::updateConnection(const HttpConnection& newHttp)
{
  if (pid.isSome()) {
    // Wipe the PID if this is an upgrade from PID to HTTP.
    // TODO(benh): unlink(oldPid);
    pid = None();
  } else if (http.isSome()) {
    // Cleanup the old HTTP connection.
    // Note that master creates a new HTTP connection for every
    // subscribe request, so 'newHttp' should always be different
    // from 'http'.
    closeHttpConnection();
  }

  CHECK_NONE(http);

  http = newHttp;
}


void Framework::closeHttpConnection()
{
  CHECK_SOME(http);

  if (connected() && !http->close()) {
    LOG(WARNING) << "Failed to close HTTP pipe for " << *this;
  }

  http = None();

  CHECK_SOME(heartbeater);

  terminate(heartbeater->get());
  wait(heartbeater->get());

  heartbeater = None();
}


void Framework::heartbeat()
{
  CHECK_NONE(heartbeater);
  CHECK_SOME(http);

  // TODO(vinod): Make heartbeat interval configurable and include
  // this information in the SUBSCRIBED response.
  scheduler::Event event;
  event.set_type(scheduler::Event::HEARTBEAT);

  heartbeater =
    new Heartbeater<scheduler::Event, v1::scheduler::Event>(
        "framework " + stringify(info.id()),
        event,
        http.get(),
        DEFAULT_HEARTBEAT_INTERVAL,
        None(),
        [this](const scheduler::Event& event) {
          this->metrics.incrementEvent(event);
        });

  process::spawn(heartbeater->get());
}


bool Framework::isTrackedUnderRole(const std::string& role) const
{
  CHECK(master->isWhitelistedRole(role))
    << "Unknown role '" << role << "'" << " of framework " << *this;

  return master->roles.contains(role) &&
         master->roles.at(role)->frameworks.contains(id());
}


void Framework::trackUnderRole(const std::string& role)
{
  CHECK(master->isWhitelistedRole(role))
    << "Unknown role '" << role << "'" << " of framework " << *this;

  CHECK(!isTrackedUnderRole(role));

  if (!master->roles.contains(role)) {
    master->roles[role] = new Role(role);
  }
  master->roles.at(role)->addFramework(this);
}


void Framework::untrackUnderRole(const std::string& role)
{
  CHECK(master->isWhitelistedRole(role))
    << "Unknown role '" << role << "'" << " of framework " << *this;

  CHECK(isTrackedUnderRole(role));

  // NOTE: Ideally we would also `CHECK` that we're not currently subscribed
  // to the role. We don't do this currently because this function is used in
  // `Master::removeFramework` where we're still subscribed to `roles`.

  auto allocatedToRole = [&role](const Resource& resource) {
    return resource.allocation_info().role() == role;
  };

  CHECK(totalUsedResources.filter(allocatedToRole).empty());
  CHECK(totalOfferedResources.filter(allocatedToRole).empty());

  master->roles.at(role)->removeFramework(this);
  if (master->roles.at(role)->frameworks.empty()) {
    delete master->roles.at(role);
    master->roles.erase(role);
  }
}


void Framework::setFrameworkState(const Framework::State& _state)
{
  state = _state;
  metrics.subscribed = state == Framework::State::ACTIVE ? 1 : 0;
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
