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

#ifndef __MASTER_ALLOCATOR_MESOS_METRICS_HPP__
#define __MASTER_ALLOCATOR_MESOS_METRICS_HPP__

#include <string>
#include <vector>

#include <mesos/quota/quota.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/pull_gauge.hpp>
#include <process/metrics/push_gauge.hpp>
#include <process/metrics/timer.hpp>

#include <process/pid.hpp>

#include <stout/hashmap.hpp>

namespace mesos {
namespace internal {
namespace master {
namespace allocator {
namespace internal {

// Forward declarations.
class HierarchicalAllocatorProcess;

// Collection of metrics for the allocator; these begin
// with the following prefix: `allocator/mesos/`.
struct Metrics
{
  explicit Metrics(const HierarchicalAllocatorProcess& allocator);

  ~Metrics();

  void setQuota(const std::string& role, const Quota& quota);
  void removeQuota(const std::string& role);

  void addRole(const std::string& role);
  void removeRole(const std::string& role);

  const process::PID<HierarchicalAllocatorProcess> allocator;

  // Number of dispatch events currently waiting in the allocator process.
  process::metrics::PullGauge event_queue_dispatches;

  // TODO(bbannier) This metric is identical to `event_queue_dispatches`, but
  // uses a name deprecated in 1.0. This metric should be removed after the
  // deprecation cycle.
  process::metrics::PullGauge event_queue_dispatches_;

  // Number of times the allocation algorithm has run.
  process::metrics::Counter allocation_runs;

  // Time spent in the allocation algorithm.
  process::metrics::Timer<Milliseconds> allocation_run;

  // The latency of allocation runs due to the batching of allocation requests.
  process::metrics::Timer<Milliseconds> allocation_run_latency;

  // PullGauges for the total amount of each resource in the cluster.
  std::vector<process::metrics::PullGauge> resources_total;

  // PullGauges for the allocated amount of each resource in the cluster.
  std::vector<process::metrics::PullGauge> resources_offered_or_allocated;

  // PullGauges for the per-role quota allocation for each resource.
  hashmap<std::string, hashmap<std::string, process::metrics::PullGauge>>
    quota_allocated;

  // PullGauges for the per-role quota guarantee for each resource.
  hashmap<std::string, hashmap<std::string, process::metrics::PullGauge>>
    quota_guarantee;

  // PullGauges for the per-role count of active offer filters.
  hashmap<std::string, process::metrics::PullGauge> offer_filters_active;
};


struct FrameworkMetrics
{
  explicit FrameworkMetrics(const FrameworkInfo& _frameworkInfo);

  ~FrameworkMetrics();

  void reviveRole(const std::string& role);
  void suppressRole(const std::string& role);

  // Since frameworks can update their list of roles,
  // these methods add/remove per-role metrics.
  void addSubscribedRole(const std::string& role);
  void removeSubscribedRole(const std::string& role);

  const FrameworkInfo frameworkInfo;

  // Suppresion state metric (boolean 0 or 1) for each role.
  hashmap<std::string, process::metrics::PushGauge> suppressed;
};

} // namespace internal {
} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_ALLOCATOR_MESOS_METRICS_HPP__
