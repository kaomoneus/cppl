//===--------------------- SolvedDependenciesInfo.h ------------------*- C++ -*-===//
//
// Part of the C++ Levitation Project,
// under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines bidirectional Solved Dependencies Info
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LEVITATION_SOLVED_DEPENDENCIES_INFO_H
#define LLVM_LEVITATION_SOLVED_DEPENDENCIES_INFO_H

#include "clang/Levitation/DependenciesSolver/DependenciesGraph.h"
#include "clang/Levitation/Common/Failable.h"
#include "clang/Levitation/Common/WithOperator.h"
#include "clang/Levitation/Serialization.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <assert.h>
#include <memory>
#include <map>
#include <utility>

namespace clang { namespace levitation { namespace dependencies_solver {

class SolvedDependenciesInfo : public Failable {

  using NodeID = DependenciesGraph::NodeID;
  using Node = DependenciesGraph::Node;
  using NodesSet = DependenciesGraph::NodesSet;

  typedef DenseMap<NodeID::Type, size_t> TopologicallyOrderedNodesTy;
  TopologicallyOrderedNodesTy TopologicallyOrderedNodes;

  typedef DenseMap<NodeID::Type, size_t /*distance from terminal*/> PathTy;
  typedef SmallVector<PathTy, 4> CyclesTy;

public:
  using RangedDependenciesMap = std::map<size_t, NodeID::Type>;
  struct NodeStackInfo {
    size_t StackSize;
    RangedDependenciesMap FullDependencies;
  };

  using FullDepsMapTy =
      llvm::DenseMap<NodeID::Type, NodeStackInfo>;

  using OnDiagCyclesFoundFn = std::function<void(
      const RangedDependenciesMap &, NodeID::Type
  )>;

private:
  std::shared_ptr<DependenciesGraph> DGraph;
  FullDepsMapTy FullDepsMap;
  CyclesTy Cycles;

  SolvedDependenciesInfo(
      std::shared_ptr<DependenciesGraph> &&DGraph
  )
  : DGraph(std::move(DGraph))
  {}

  void addCycle(const PathTy &C) {
    static const size_t MAX_CYCLES = 10;
    if (Cycles.size() >= MAX_CYCLES) return;

    Cycles.push_back(C);
  }

  /// Finds topological order by means of DFS, and for each node
  /// collects direct or indirect dependencies set (topologically ordered deps, aka TOD).
  /// Note, there is no stack of topological ordered nodes. It's only
  /// current stack size of that stack (Range) we need to create TOD for each node.
  /// \param G Graph method operates on
  /// \param N Current node
  /// \param DistFromTerm distance from terminal node, which was used as root
  ///        for recursive call. Is required for diagnostics, for proper cycles info.
  /// \param StackSize distance from leaf node, equal to stack size which is used in
  ///        classical DFS topological ordering algorithm.
  /// \param CycleCandidate Set of nodes which are currently participate
  ///        recursive call chain. If on some call we bump into duplicate,
  ///        then it's a cycle.
  void dfsSolve(
      const DependenciesGraph &G,
      const Node &N,
      size_t DistFromTerm,
      size_t &StackSize,
      PathTy& CycleCandidate
  ) {

#if 1
    static auto &Strings = CreatableSingleton<DependenciesStringsPool>::get();
    static auto &Trace = log::Logger::get().verbose();

    Trace.indent(DistFromTerm)
    << "dfsSolve for "; G.dumpNodeShort(Trace, N.ID, Strings);
    Trace << "\n";
#endif

    // Detect cycles if any.
    auto InsCycle = CycleCandidate.insert({N.ID, DistFromTerm});
    if (!InsCycle.second) {
      addCycle(CycleCandidate);
      return;
    }

    // Guard CycleCandidate, so current node would be removed automatically,
    // once we exit this call.
    with (auto _ = on_exit([&] { CycleCandidate.erase(N.ID); }))
    {
      // Don't go through already visited nodes.
      auto InsRes = FullDepsMap.insert({N.ID, NodeStackInfo()});
      if (!InsRes.second)
        return;

      // Go over all dependency nodes and do DFS.

      size_t NextDistFromTerm = DistFromTerm + 1;

      for (auto InNID : N.Dependencies) {
        const auto &InN = G.getNode(InNID);

        dfsSolve(G, InN, NextDistFromTerm, StackSize, CycleCandidate);

        assert(
          FullDepsMap.count(InNID) &&
          "Info for all in Nodes should be added it deeper recursive calls"
        );

        // As long as we're working with DenseMap, its contents invalidated
        // after each insertion, and thus if we would obtain NodeInfo->FullDeps before loop
        // with dfs calls we couldn't rely on its contents all the way.
        auto &FullDeps = FullDepsMap[N.ID].FullDependencies;

        const auto &InNodeInfo = FullDepsMap[InNID];
        const auto &InNodeFullDeps = InNodeInfo.FullDependencies;
        for (const auto &InDepkv : InNodeFullDeps) {
          FullDeps.insert(InDepkv);
#if 1
          Trace.indent(DistFromTerm) << " copy chain item { " << InDepkv.first << " : ";
          G.dumpNodeShort(Trace, InDepkv.second, Strings);
          Trace.indent(DistFromTerm) << " }\n";
#endif
        }
#if 1
        Trace.indent(DistFromTerm) << " insert { " << InNodeInfo.StackSize << " : ";
        G.dumpNodeShort(Trace, InNID, Strings);
        Trace.indent(DistFromTerm) << " }\n";
#endif
        FullDeps.insert({InNodeInfo.StackSize, InNID});
      }

      FullDepsMap[N.ID].StackSize = StackSize++;
#if 1
      Trace.indent(DistFromTerm) << "Stack size " << StackSize << "\n";
#endif
    }
  }

  void dfsSolveRoot(const DependenciesGraph &G, const Node &N, size_t &Range) {
    PathTy CycleCandidate;
    dfsSolve(G, N, 0, Range, CycleCandidate);
  }

  void findCycles(const DependenciesGraph &G, const NodesSet &SubGraph) {
    NodesSet Visited;
    for (auto &NID : SubGraph) {
      if (!Visited.count(NID)) {
        PathTy CycleCandidate;
        findCyclesDfs(G, SubGraph, NID, Visited, 0, CycleCandidate);
      }
    }
  }

  void findCyclesDfs(
      const DependenciesGraph &G,
      const NodesSet &SubGraph,
      NodeID::Type NID,
      NodesSet &Visited,
      size_t DistFromRoot,
      PathTy &CycleCandidate
  ) {
    auto InsCycle = CycleCandidate.insert({NID, DistFromRoot});
    if (!InsCycle.second) {
      addCycle(CycleCandidate);
      return;
    }

    with (auto _ = on_exit([&] { CycleCandidate.erase(NID); })) {
      auto InsRes = Visited.insert(NID);
      if (!InsRes.second)
        return;

      auto &N = G.getNode(NID);
      auto NextDist = DistFromRoot + 1;

      for (auto InNID : N.Dependencies) if (SubGraph.count(InNID))
        findCyclesDfs(G, SubGraph, InNID, Visited, NextDist, CycleCandidate);
    }
  }

  void findIsolatedCycles(const DependenciesGraph &G) {
    const auto &AllNodes = G.allNodes();
    NodesSet IsolatedCycles;

    if (TopologicallyOrderedNodes.size() != AllNodes.size()) {
      for (const auto &Nkv : AllNodes) {
        auto NID = Nkv.first;
        if (!TopologicallyOrderedNodes.count(NID))
          IsolatedCycles.insert(NID);
      }
      findCycles(G, IsolatedCycles);
    }
  }

  void build() {
    const auto &G = getDependenciesGraph();

    size_t Range = 0;

    for (auto &NID : G.terminals())
      dfsSolveRoot(G, G.getNode(NID), Range);

    findIsolatedCycles(G);

    if (!Cycles.empty()) {
      setFailure("Found cycles.");
      return;
    }
  }

public:

  static std::shared_ptr<SolvedDependenciesInfo> build(
      std::shared_ptr<DependenciesGraph> Graph
  ) {
    std::shared_ptr<SolvedDependenciesInfo>
        Info(new SolvedDependenciesInfo(std::move(Graph)));
    Info->build();
    return Info;
  }

  const DependenciesGraph &getDependenciesGraph() const {
    return *DGraph;
  }

  const FullDepsMapTy &getDependenciesMap() const {
    return FullDepsMap;
  }

  const RangedDependenciesMap &getRangedDependencies(NodeID::Type NID) const {
    static RangedDependenciesMap EmptyMap;

    auto Found = FullDepsMap.find(NID);

    if (Found == FullDepsMap.end())
      return EmptyMap;

    return Found->second.FullDependencies;
  }

  void dump(
      llvm::raw_ostream &out,
      const DependenciesStringsPool &Strings
  ) const {

    auto &DGraphRef = *DGraph;

    DGraphRef.bsfWalkSkipVisited([&](const Node &N) {
      auto NID = N.ID;
      const RangedDependenciesMap &FullDeps = getRangedDependencies(NID);

      const auto &Path = *Strings.getItem(N.LevitationUnit->PackagePath);
      out << "[";
        DGraphRef.dumpNodeID(out, NID);
      out << "]\n";

      out << "    Path: " << Path << "\n";
      if (FullDeps.size()) {
        out
                << "    Full dependencies:\n";

        for (const auto &DepSzNID : FullDeps) {
          const auto &Dep = DGraphRef.getNode(DepSzNID.second);
          const auto &DepPath = *Strings.getItem(Dep.LevitationUnit->PackagePath);

          out.indent(8) << "[";
          DGraphRef.dumpNodeID(out, DepSzNID.second);
          out
                  << "]: " << DepPath << "\n";
        }

        out
                << "    Direct dependencies:\n";

        for (const auto &DepID : N.Dependencies) {
          const auto &Dep = DGraphRef.getNode(DepID);
          const auto &DepPath = *Strings.getItem(Dep.LevitationUnit->PackagePath);

          out
                  << "        " << DepPath << "\n";
        }
      } else {
        out << "    (root)\n";
      }
    });

    // Dump cycles if any.
    size_t ci = 0;
    for (const PathTy &C : Cycles) {
      out << "Cycle #" << ci++ << "\n";

      using kv_t = std::pair<PathTy::key_type, PathTy::mapped_type>;
      SmallVector<kv_t, 16> Path;
      for (const auto &NIDRange : C) Path.push_back(NIDRange);

      std::sort(
          Path.begin(),
          Path.end(),
          [&] (const kv_t &L, const kv_t &R) {
        return L.second < R.second;
      });

      for (auto NIDRange : Path) {
        out << "  ";
        DGraphRef.dumpNodeShort(out, NIDRange.first, Strings);
        out << "\n";
      }
    }
  }
};

}}} // end of clang::levitation::dependencies_solver namespace

#endif //LLVM_LEVITATION_SOLVED_DEPENDENCIES_INFO_H
