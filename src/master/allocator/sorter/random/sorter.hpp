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

#ifndef __MASTER_ALLOCATOR_SORTER_RANDOM_SORTER_HPP__
#define __MASTER_ALLOCATOR_SORTER_RANDOM_SORTER_HPP__

#include <algorithm>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>
#include <mesos/values.hpp>

#include <stout/check.hpp>
#include <stout/hashmap.hpp>
#include <stout/option.hpp>

#include "common/resource_quantities.hpp"

#include "master/allocator/sorter/sorter.hpp"


namespace mesos {
namespace internal {
namespace master {
namespace allocator {

class RandomSorter : public Sorter
{
public:
  RandomSorter();

  explicit RandomSorter(
      const process::UPID& allocator,
      const std::string& metricsPrefix);

  ~RandomSorter() override;

  void initialize(
      const Option<std::set<std::string>>& fairnessExcludeResourceNames)
    override;

  void add(const std::string& clientPath) override;

  void remove(const std::string& clientPath) override;

  void activate(const std::string& clientPath) override;

  void deactivate(const std::string& clientPath) override;

  void updateWeight(const std::string& path, double weight) override;

  void allocated(
      const std::string& clientPath,
      const SlaveID& slaveId,
      const Resources& resources) override;

  void update(
      const std::string& clientPath,
      const SlaveID& slaveId,
      const Resources& oldAllocation,
      const Resources& newAllocation) override;

  void unallocated(
      const std::string& clientPath,
      const SlaveID& slaveId,
      const Resources& resources) override;

  const hashmap<SlaveID, Resources>& allocation(
      const std::string& clientPath) const override;

  const Resources& allocationScalarQuantities(
      const std::string& clientPath) const override;

  hashmap<std::string, Resources> allocation(
      const SlaveID& slaveId) const override;

  Resources allocation(
      const std::string& clientPath,
      const SlaveID& slaveId) const override;

  const Resources& totalScalarQuantities() const override;

  void add(const SlaveID& slaveId, const Resources& resources) override;

  void remove(const SlaveID& slaveId, const Resources& resources) override;

  // This will perform a weighted random shuffle on each call.
  //
  // TODO(bmahler): Unlike the DRF sorter, the allocator ideally would
  // not call `sort()` for every agent, but rather loop through a single
  // weighted shuffle before re-shuffling..
  std::vector<std::string> sort() override;

  bool contains(const std::string& clientPath) const override;

  size_t count() const override;

private:
  // A node in the sorter's tree.
  struct Node;

  // Returns the weight associated with the node. If no weight has
  // been configured for the node's path, the default weight (1.0) is
  // returned.
  double getWeight(const Node* node) const;

  // Returns the client associated with the given path. Returns
  // nullptr if the path is not found or if the path identifies an
  // internal node in the tree (not a client).
  Node* find(const std::string& clientPath) const;

  // Used for random number generation.
  std::mt19937 generator;

  // The root node in the sorter tree.
  Node* root;

  // To speed lookups, we keep a map from client paths to the leaf
  // node associated with that client. There is an entry in this map
  // for every leaf node in the client tree (except for the root when
  // the tree is empty). Paths in this map do NOT contain the trailing
  // "." label we use for leaf nodes.
  hashmap<std::string, Node*> clients;

  // Weights associated with role paths. Setting the weight for a path
  // influences the sampling probability of all nodes in the subtree
  // rooted at that path. This hashmap might include weights for paths
  // that are not currently in the sorter tree.
  hashmap<std::string, double> weights;

  // Total resources.
  struct Total
  {
    // We need to keep track of the resources (and not just scalar
    // quantities) to account for multiple copies of the same shared
    // resources. We need to ensure that we do not update the scalar
    // quantities for shared resources when the change is only in the
    // number of copies in the sorter.
    hashmap<SlaveID, Resources> resources;

    // NOTE: Scalars can be safely aggregated across slaves. We keep
    // that to speed up the calculation of shares. See MESOS-2891 for
    // the reasons why we want to do that.
    //
    // NOTE: We omit information about dynamic reservations and
    // persistent volumes here to enable resources to be aggregated
    // across slaves more effectively. See MESOS-4833 for more
    // information.
    //
    // Sharedness info is also stripped out when resource identities
    // are omitted because sharedness inherently refers to the
    // identities of resources and not quantities.
    Resources scalarQuantities;

    // To improve the performance of calculating shares, we store
    // a redundant but more efficient version of `scalarQuantities`.
    // See MESOS-4694.
    //
    // TODO(bmahler): Can we remove `scalarQuantities` in favor of
    // using this type whenever scalar quantities are needed?
    ResourceQuantities totals;
  } total_;
};


// Represents a node in the sorter's tree. The structure of the tree
// reflects the hierarchical relationships between the clients of the
// sorter. Some (but not all) nodes correspond to sorter clients; some
// nodes only exist to represent the structure of the sorter
// tree. Clients are always associated with leaf nodes.
//
// For example, if there are two sorter clients "a/b" and "c/d", the
// tree will contain five nodes: the root node, internal nodes for "a"
// and "c", and leaf nodes for the clients "a/b" and "c/d".
struct RandomSorter::Node
{
  // Indicates whether a node is an active leaf node, an inactive leaf
  // node, or an internal node. Sorter clients always correspond to
  // leaf nodes, and only leaf nodes can be activated or deactivated.
  // The root node is always an "internal" node.
  enum Kind
  {
    ACTIVE_LEAF,
    INACTIVE_LEAF,
    INTERNAL
  };

  Node(const std::string& _name, Kind _kind, Node* _parent)
    : name(_name), kind(_kind), parent(_parent)
  {
    // Compute the node's path. Three cases:
    //
    //  (1) If the root node, use the empty string
    //  (2) If a child of the root node, use the child's name
    //  (3) Otherwise, use the parent's name, "/", and the child's name.
    if (parent == nullptr) {
      path = "";
    } else if (parent->parent == nullptr) {
      path = name;
    } else {
      path = strings::join("/", parent->path, name);
    }
  }

  ~Node()
  {
    foreach (Node* child, children) {
      delete child;
    }
  }

  // The label of the edge from this node's parent to the
  // node. "Implicit" leaf nodes are always named ".".
  //
  // TODO(neilc): Consider naming implicit leaf nodes in a clearer
  // way, e.g., by making `name` an Option?
  std::string name;

  // Complete path from root to node. This includes the trailing "."
  // label for virtual leaf nodes.
  std::string path;

  // Cached weight of the node, access this through `getWeight()`.
  // The value is cached by `getWeight()` and updated by
  // `updateWeight()`. Marked mutable since the caching writes
  // to this are logically const.
  mutable Option<double> weight;

  Kind kind;

  Node* parent;

  // Pointers to the child nodes. `children` is only non-empty if
  // `kind` is INTERNAL_NODE.
  //
  // All inactive leaves are stored at the end of the vector; that
  // is, each `children` vector consists of zero or more active leaves
  // and internal nodes, followed by zero or more inactive leaves. This
  // means that code that only wants to iterate over active children
  // can stop when the first inactive leaf is observed.
  std::vector<Node*> children;

  // If this node represents a sorter client, this returns the path of
  // that client. Unlike the `path` field, this does NOT include the
  // trailing "." label for virtual leaf nodes.
  //
  // For example, if the sorter contains two clients "a" and "a/b",
  // the tree will contain four nodes: the root node, "a", "a/."
  // (virtual leaf), and "a/b". The `clientPath()` of "a/." is "a",
  // because that is the name of the client associated with that
  // virtual leaf node.
  std::string clientPath() const
  {
    if (name == ".") {
      CHECK(kind == ACTIVE_LEAF || kind == INACTIVE_LEAF);
      return CHECK_NOTNULL(parent)->path;
    }

    return path;
  }

  bool isLeaf() const
  {
    if (kind == ACTIVE_LEAF || kind == INACTIVE_LEAF) {
      CHECK(children.empty());
      return true;
    }

    return false;
  }

  void removeChild(const Node* child)
  {
    // Sanity check: ensure we are removing an extant node.
    auto it = std::find(children.begin(), children.end(), child);
    CHECK(it != children.end());

    children.erase(it);
  }

  void addChild(Node* child)
  {
    // Sanity check: don't allow duplicates to be inserted.
    auto it = std::find(children.begin(), children.end(), child);
    CHECK(it == children.end());

    // If we're inserting an inactive leaf, place it at the end of the
    // `children` vector; otherwise, place it at the beginning. This
    // maintains ordering invariant above.
    if (child->kind == INACTIVE_LEAF) {
      children.push_back(child);
    } else {
      children.insert(children.begin(), child);
    }
  }

  // Allocation for a node.
  struct Allocation
  {
    Allocation() {}

    void add(const SlaveID& slaveId, const Resources& toAdd)
    {
      // Add shared resources to the allocated quantities when the same
      // resources don't already exist in the allocation.
      const Resources sharedToAdd = toAdd.shared()
        .filter([this, slaveId](const Resource& resource) {
            return !resources[slaveId].contains(resource);
        });

      const Resources quantitiesToAdd =
        (toAdd.nonShared() + sharedToAdd).createStrippedScalarQuantity();

      resources[slaveId] += toAdd;
      scalarQuantities += quantitiesToAdd;

      foreach (const Resource& resource, quantitiesToAdd) {
        totals[resource.name()] += resource.scalar();
      }
    }

    void subtract(const SlaveID& slaveId, const Resources& toRemove)
    {
      CHECK(resources.contains(slaveId));
      CHECK(resources.at(slaveId).contains(toRemove))
        << "Resources " << resources.at(slaveId) << " at agent " << slaveId
        << " does not contain " << toRemove;

      resources[slaveId] -= toRemove;

      // Remove shared resources from the allocated quantities when there
      // are no instances of same resources left in the allocation.
      const Resources sharedToRemove = toRemove.shared()
        .filter([this, slaveId](const Resource& resource) {
            return !resources[slaveId].contains(resource);
        });

      const Resources quantitiesToRemove =
        (toRemove.nonShared() + sharedToRemove).createStrippedScalarQuantity();

      foreach (const Resource& resource, quantitiesToRemove) {
        totals[resource.name()] -= resource.scalar();
      }

      CHECK(scalarQuantities.contains(quantitiesToRemove))
        << scalarQuantities << " does not contain " << quantitiesToRemove;

      scalarQuantities -= quantitiesToRemove;

      if (resources[slaveId].empty()) {
        resources.erase(slaveId);
      }
    }

    void update(
        const SlaveID& slaveId,
        const Resources& oldAllocation,
        const Resources& newAllocation)
    {
      const Resources oldAllocationQuantity =
        oldAllocation.createStrippedScalarQuantity();
      const Resources newAllocationQuantity =
        newAllocation.createStrippedScalarQuantity();

      CHECK(resources.contains(slaveId));
      CHECK(resources[slaveId].contains(oldAllocation))
        << "Resources " << resources[slaveId] << " at agent " << slaveId
        << " does not contain " << oldAllocation;

      CHECK(scalarQuantities.contains(oldAllocationQuantity))
        << scalarQuantities << " does not contain " << oldAllocationQuantity;

      resources[slaveId] -= oldAllocation;
      resources[slaveId] += newAllocation;

      scalarQuantities -= oldAllocationQuantity;
      scalarQuantities += newAllocationQuantity;

      foreach (const Resource& resource, oldAllocationQuantity) {
        totals[resource.name()] -= resource.scalar();
      }

      foreach (const Resource& resource, newAllocationQuantity) {
        totals[resource.name()] += resource.scalar();
      }
    }

    // We maintain multiple copies of each shared resource allocated
    // to a client, where the number of copies represents the number
    // of times this shared resource has been allocated to (and has
    // not been recovered from) a specific client.
    hashmap<SlaveID, Resources> resources;

    // Similarly, we aggregate scalars across slaves and omit information
    // about dynamic reservations, persistent volumes and sharedness of
    // the corresponding resource. See notes above.
    Resources scalarQuantities;

    // To improve the performance of calculating shares, we store
    // a redundant but more efficient version of `scalarQuantities`.
    // See MESOS-4694.
    //
    // TODO(bmahler): Can we remove `scalarQuantities` in favor of
    // using this type whenever scalar quantities are needed?
    ResourceQuantities totals;
  } allocation;
};

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_ALLOCATOR_SORTER_RANDOM_SORTER_HPP__
