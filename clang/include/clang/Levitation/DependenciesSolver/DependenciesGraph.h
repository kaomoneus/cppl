//===--------------------- DependenciesGraph.h ------------------*- C++ -*-===//
//
// Part of the C++ Levitation Project,
// under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines bidirectional Dependencies Graph
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LEVITATION_DEPENDENCIES_GRAPH_H
#define LLVM_LEVITATION_DEPENDENCIES_GRAPH_H

#include "clang/Levitation/DependenciesSolver/ParsedDependencies.h"

#include "clang/Levitation/Common/Path.h"
#include "clang/Levitation/Common/WithOperator.h"
#include "clang/Levitation/Common/SimpleLogger.h"
#include "clang/Levitation/Common/StringsPool.h"
#include "clang/Levitation/Common/Thread.h"
#include "clang/Levitation/TasksManager/TasksManager.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"

#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <mutex>

namespace clang { namespace levitation { namespace dependencies_solver {

class DependenciesGraph {
public:
  struct Node;

  struct LevitationUnitInfo {
    StringID PackagePath;
    Node *Declaration = nullptr;
    Node *Definition = nullptr;
    bool IsMainFile = false;
  };

  enum class NodeKind {
      Declaration,
      Definition,
      Unknown
  };

  // TODO Levitation: Rename NodeID into NodeIDUtils
  //                  NodeID::Type then may became a NodeID
  struct NodeID {
      using Type = uint64_t;

      static Type get(NodeKind Kind, StringID PathID) {

        const int NodeKindBits = 1;

        uint64_t PathIDWithoutHighestBits = (((uint64_t)~0) >> NodeKindBits) & PathID;

        return ((uint64_t)Kind << (64 - NodeKindBits) ) |
               PathIDWithoutHighestBits;
      }

      static std::pair<NodeKind, StringID> getKindAndPathID(Type ID) {
        const int NodeKindBits = 1;

        auto Kind = (NodeKind)(ID >> (64 - NodeKindBits));

        StringID PathIDWithoutHighestBits = (((uint64_t)~0) >> NodeKindBits) & ID;

        return { Kind, PathIDWithoutHighestBits };
      }

      static NodeKind getKind(Type ID) {
        NodeKind Kind;
        StringID PathID;

        std::tie(Kind, PathID) = getKindAndPathID(ID);

        return Kind;
      }
  };

  using NodesMap = llvm::DenseMap<NodeID::Type, std::unique_ptr<Node>>;
  using NodesSet = llvm::DenseSet<NodeID::Type>;
  using NodesList = llvm::SmallVector<NodeID::Type, 16>;
  using UnitsMap = llvm::DenseMap<StringID, std::unique_ptr<LevitationUnitInfo>>;

  struct Node {
    Node(NodeID::Type id, NodeKind kind)
    : ID(id), Kind(kind), LevitationUnit(nullptr)
    {}
    NodeID::Type ID;
    NodeKind Kind;
    LevitationUnitInfo* LevitationUnit;
    NodesSet Dependencies;
    NodesSet DependentNodes;
  };

private:

  log::Logger &Log = log::Logger::get();

  /// Nodes whithout dependencies.
  NodesSet Roots;

  /// Graph terminal nodes. Contains nodes without dependent nodes.
  /// Should be used as starting points from build process.
  NodesSet Terminals;

  /// Last nodes in deps chain which are marked as public.
  NodesSet PublicTerminals;

  /// All nodes in graph which correspond to public ones.
  /// So far we expect only declaration nodes to be present here.
  NodesSet PublicNodes;

  /// All nodes in graph which correspond to external nodes.
  /// So far we expect only declaration nodes to be present here.
  NodesSet ExternalNodes;

  NodesMap AllNodes;
  UnitsMap PackageInfos;

  bool Invalid = false;

  /// Mark node as publicly available (present in library interface)
  /// \param NID Node ID to be marked.
  void setPublic(NodeID::Type NID) { PublicNodes.insert(NID); }

  /// Mark node as publicly available (present in library interface)
  /// \param NID Node ID to be marked.
  void setExternal(NodeID::Type NID) { ExternalNodes.insert(NID); }

public:

  static std::shared_ptr<DependenciesGraph> build(
      const ParsedDependencies &ParsedDeps,
      const DenseSet<StringID> &ExternalPackages
  ) {
    auto DGraphPtr = std::make_shared<DependenciesGraph>();
    auto &Log = DGraphPtr->Log;

    Log.verbose() << "Building dependencies graph...\n";

    for (auto &PackageDeps : ParsedDeps) {

      auto PackagePathID = PackageDeps.first;
      DependenciesData &PackageDependencies = *PackageDeps.second;
      bool IsExternal = ExternalPackages.count(PackagePathID);
      bool IsBodyOnly = PackageDependencies.IsBodyOnly;


      Log.log_trace("Creating package for Package #", PackagePathID);

      LevitationUnitInfo &Package = DGraphPtr->createPackageInfo(
        PackagePathID, IsExternal, IsBodyOnly
      );

      // Make sure previous function created correct package
      assert((!IsBodyOnly) == (bool)Package.Declaration);
      assert((!IsExternal) == (bool)Package.Definition);

      // If node has no dependencies we add it into the Roots
      // collection.

      if (PackageDependencies.DeclarationDependencies.empty()) {

        // If declaration doesn't depend on anything, then make it root.
        if (!IsBodyOnly)
          DGraphPtr->Roots.insert(Package.Declaration->ID);

        // Additionally if relevant definition doesn't depent on anything too,
        // then make it root as well.
        if (
          PackageDependencies.DefinitionDependencies.empty() && !IsExternal
        )
          DGraphPtr->Roots.insert(Package.Definition->ID);
      }

      if (!IsBodyOnly) {
        DGraphPtr->addDependenciesTo(
            *Package.Declaration,
            PackageDependencies.DeclarationDependencies
        );
      }

      if (!IsExternal) {
        // For definition we have to add both declaration and definition
        // dependencies.

        if (!IsBodyOnly) {
          DGraphPtr->addDependenciesTo(
              *Package.Definition,
              PackageDependencies.DeclarationDependencies
          );
        }

        DGraphPtr->addDependenciesTo(
          *Package.Definition,
          PackageDependencies.DefinitionDependencies
        );
      }

      if (PackageDependencies.IsPublic) {
        Log.log_trace("(declaration is loaded as public)");
        DGraphPtr->setPublic(Package.Declaration->ID);
      }

      if (IsExternal)
        DGraphPtr->setExternal(Package.Declaration->ID);
    }

    if (!DGraphPtr->AllNodes.empty() && DGraphPtr->Roots.empty()) {
      DGraphPtr->Invalid = true;
    }

    // Scan for regular terminal nodes
    DGraphPtr->collectTerminals();

    // Look for cycles and may be even fix them.
    // Note: cycles processing starts from
    // terminals.
    DGraphPtr->processCycles();

    // Scan for publically available terminal nodes.
    DGraphPtr->collectPublicNodes();

    return DGraphPtr;
  }

  /// Whether node is public
  /// \param NID Node ID to be checked
  /// \return true if node should be present in library public interface
  bool isPublic(NodeID::Type NID) const { return PublicNodes.count(NID); }

  /// Whether node belongs to external package.
  /// \param NID Node ID to be checked
  /// \return true if external
  bool isExternal(NodeID::Type NID) const { return ExternalNodes.count(NID); }

  // TODO Levitation: That looks pretty much like A* walk.
  //
  void bsfWalkSkipVisited(
       std::function<void(const Node &)> &&OnNode) const {
    NodesSet Visited;
    bsfWalk(Visited, true, [&] (const Node &N) { OnNode(N); return true; });
  }

  void bsfWalkSkipVisited(
       NodesSet &Visited,
       std::function<void(const Node &)> &&OnNode) const {
    bsfWalk(Visited, true, [&] (const Node &N) { OnNode(N); return true; });
  }

  bool bsfWalk(std::function<bool(const Node &)> &&OnNode) const {
    NodesSet Visited;
    return bsfWalk(Visited, false, std::move(OnNode));
  }

  /// Implements deep search first walk, and runs job on each node
  /// it meets.
  /// Starts from terminal nodes going down to roots.
  /// \param OnNode Action to be launched to process current node
  ///        but not its subnodes.
  /// \return true is walk was successful.
  bool dsfJobs(
      const NodesSet& StartingPoints,
      std::function<bool(const Node&)> &&OnNode
  ) const {
    JobsContext Jobs(std::move(OnNode));

    std::mutex VisitedMutex;
    NodesSet Visited;

    return dsfJobsOnNode(
        VisitedMutex,
        Visited,
        nullptr,
        StartingPoints,
        Jobs
    );
  }

  bool dsfJobs(
      std::function<bool(const Node&)> &&OnNode
  ) const {
    return dsfJobs(Terminals, std::move(OnNode));
  }

  bool isInvalid() const { return Invalid; }

  // Do BFS graph walk and dump each node
  void dump(
      llvm::raw_ostream &out, const DependenciesStringsPool &Strings
  ) const {

    if (Roots.empty()) {
      out << "(empty)\n\n";
      return;
    }

    NodesSet Visited;
    bsfWalkSkipVisited(Visited, [&](const Node &N) {
        dumpNode(out, N.ID, Strings);
        out << "\n";
    });

    // If graph has cycles it may be possible that
    // we have non-empty graph and empty Terminals collection.
    // Don't fire error yet, do it later during dependency solve
    // stage.
    if (Terminals.empty()) {
      out << "No terminal nodes found. Graph has cycles.\n";
      return;
    }

    out << "Terminals:\n";
    for (auto TerminalNodeID : Terminals) {
      out << "    ";
      dumpNodeID(out, TerminalNodeID);
      out << "\n";
    }
    out << "\n";

    if (Visited.size() < AllNodes.size()) {
      out << "Isolated nodes:\n";
      for (const auto &NKV : AllNodes) {
        if (!Visited.count(NKV.first)) {
          dumpNode(out, NKV.first, Strings);
          out << "\n";
        }
      }
    }
  }

  void dumpNode(
      llvm::raw_ostream &out,
      NodeID::Type NodeID,
      const DependenciesStringsPool &Strings
  ) const {

    const Node &Node = getNode(NodeID);

    const auto &PackagePathStr = Node.LevitationUnit ?
        *Strings.getItem(Node.LevitationUnit->PackagePath) :
        *Strings.getItem(NodeID::getKindAndPathID(Node.ID).second);

    out
    << "Node";

    if (Roots.count(NodeID))
      out << "(root)";

    out
    << "[";
    dumpNodeID(out, NodeID);
    out << "], "
        << PackagePathStr << ":\n";

    if (!Node.LevitationUnit)
      out << "  ERROR: NO PACKAGE INFO, Path is recovered from Node ID\n"

    << "    Path: " << PackagePathStr << "\n"
    << "    Kind: "
    << (Node.Kind == NodeKind::Declaration ? "Declaration" : "Definition") << "\n";

    if (Node.DependentNodes.size()) {
      out << "    Is used by:\n";
      for (auto DependentNodeID : Node.DependentNodes) {
        out << "        ";
        dumpNodeID(out, DependentNodeID);
        out << "\n";
      }
    }

    if (Node.Dependencies.size()) {
      out << "    Dependencies:\n";
      for (auto DependencyID : Node.Dependencies) {
        out << "        ";
        dumpNodeID(out, DependencyID);
        out << "\n";
      }
    }
  }

  void dumpNodeShort(
      llvm::raw_ostream &out,
      NodeID::Type NodeID,
      const DependenciesStringsPool &Strings
  ) const {

    const Node &Node = getNode(NodeID);

    out
    << "Node[";
    dumpNodeID(out, NodeID);
    out << "]: " << *Strings.getItem(Node.LevitationUnit->PackagePath);
  }

  std::string nodeDescrShort(
      NodeID::Type NodeID,
      const DependenciesStringsPool &Strings
  ) const {
    std::string Descr;
    llvm::raw_string_ostream out(Descr);
    dumpNodeShort(out, NodeID, Strings);
    return Descr;
  }

  void dumpNodeID(llvm::raw_ostream &out, NodeID::Type NodeID) const {
    NodeKind Kind;
    StringID PathID;
    std::tie(Kind, PathID) = NodeID::getKindAndPathID(NodeID);

    out
    << PathID << ":"
    << (Kind == NodeKind::Declaration ? "DECL" : "DEF");

    SmallVector<StringRef, 4> Flags;

    if (isPublic(NodeID))
      Flags.push_back("public");

    size_t FlagsSize = Flags.size();
    if (FlagsSize) {
      out << "(";
      for (unsigned i = 0, e = FlagsSize-1; i != e; ++i)
        out << Flags[i] << ", ";
      out << Flags[FlagsSize-1];
      out << ")";
    }
  }

  Node &getNode(NodeID::Type ID) {
    auto Found = AllNodes.find(ID);
    assert(
        Found != AllNodes.end() &&
        "Node with current ID should be present in AllNodes"
    );
    return *Found->second;
  }

  const Node &getNode(NodeID::Type ID) const {
    auto Found = AllNodes.find(ID);
    assert(
        Found != AllNodes.end() &&
        "Node with current ID should be present in AllNodes"
    );
    return *Found->second;
  }

  const NodesMap &allNodes() const { return AllNodes; }
  const NodesSet &roots() const { return Roots; }
  const NodesSet &terminals() const { return Terminals; }

protected:

  using OnNodeFn = std::function<bool(const Node&)>;

  bool bsfWalk(
      NodesSet &VisitedNodes,
      bool SkipVisited,
      OnNodeFn &&OnNode
  ) const {
    NodesSet Worklist = Roots;

    NodesSet NewWorklist;

    while (Worklist.size()) {
      NewWorklist.clear();
      for (auto NID : Worklist) {

        if (SkipVisited && !VisitedNodes.insert(NID).second)
          continue;

        const auto &Node = getNode(NID);

        if (!OnNode(Node))
          return false;

        NewWorklist.insert(Node.DependentNodes.begin(), Node.DependentNodes.end());
      }
      Worklist.swap(NewWorklist);
    }
    return true;
  }

  class JobsContext {

    using TasksMapType =
        llvm::DenseMap<NodeID::Type, tasks::TasksManager::TaskID>;

    TasksMapType Tasks;

    OnNodeFn OnNode;
    std::mutex Mutex;

    MutexLock lockTasks() {
      return lock(Mutex);
    }

  public:

    JobsContext(OnNodeFn &&onNode) : OnNode(onNode) {}

    tasks::TasksManager::TaskID getJobForNode(
        NodeID::Type NID,
        tasks::TasksManager::ActionFn &&Fn,
        bool SameThread
    ) {
      auto &TM = tasks::TasksManager::get();
      auto TID = SameThread ?
          TM.addTask(std::move(Fn), true) :
          TM.runTask(std::move(Fn));

      auto _ = lockTasks();
      auto Res = Tasks.insert({NID, TID});
      return Res.first->second;
    }

    bool onNode(const Node &N) {
      return OnNode(N);
    }
  };

  bool dsfJobsOnNode(
      std::mutex &VisitedMutex,
      NodesSet &Visited,
      const Node *N,
      const NodesSet &SubNodes,
      JobsContext &Jobs
  ) const {

    bool Successful = true;

    if (SubNodes.size()) {

      tasks::TasksManager::TasksSet NodeTasks;

      // Some tricky way to go over DenseMap, and
      // detect the last node in collection
      size_t NumSubNodes = SubNodes.size();
      size_t SubNodeIdx = 0;

      for (auto NID : SubNodes) {
        const auto &SubNode = getNode(NID);

        with (auto _ = lock(VisitedMutex)) {
          auto VisitIt = Visited.insert(NID);
          if (!VisitIt.second)
            continue;
        }

        auto TID = Jobs.getJobForNode(
            SubNode.ID,
            [&](tasks::TasksManager::TaskContext &TC) {
              TC.Successful = dsfJobsOnNode(
                  VisitedMutex,
                  Visited,
                  &SubNode,
                  SubNode.Dependencies,
                  Jobs
              );
            },
            // Process last subnode in same thread
            /*Same thread*/++SubNodeIdx == NumSubNodes
        );

        NodeTasks.insert(TID);

        auto &TM = tasks::TasksManager::get();

        TM.waitForTasks(NodeTasks);
        Successful = TM.allSuccessfull(NodeTasks);
      }
    }

    if (Successful && N)
      Successful = Jobs.onNode(*N);

    return Successful;
  }


  void addDependenciesTo(
      Node &DependentNode,
      const DependenciesData::DeclarationsBlock &Dependencies
  ) {
    auto DependentNodeID = DependentNode.ID;

    for (auto &Dep : Dependencies) {
      Node &DeclDependencyNode = getOrCreateNode(
          NodeKind::Declaration,
          Dep.UnitIdentifier
      );

      DependentNode.Dependencies.insert(DeclDependencyNode.ID);
      DeclDependencyNode.DependentNodes.insert(DependentNodeID);
    }
  }

  LevitationUnitInfo &createPackageInfo(
      StringID PackagePathID,
      bool IsExternal,
      bool IsBodyOnly
  ) {

    auto PackageRes = PackageInfos.insert({
      PackagePathID, std::make_unique<LevitationUnitInfo>()
    });

    LevitationUnitInfo &Package = *PackageRes.first->second;

    assert(
        PackageRes.second &&
        "Only one package can be created for particular PackagePathID"
    );

    Package.PackagePath = PackagePathID;

    if (!IsBodyOnly) {
      Node &DeclNode = getOrCreateNode(NodeKind::Declaration, PackagePathID);

      DeclNode.LevitationUnit = &Package;
      Package.Declaration = &DeclNode;
    }

    if (!IsExternal) {
      Node &DefNode = getOrCreateNode(NodeKind::Definition, PackagePathID);
      DefNode.LevitationUnit = &Package;
      Package.Definition = &DefNode;
    }

    // Note, that we don't make definition node to be dependent on
    // declaration node, the reason is how we build definition:
    // per current implementation we compile whole source again,
    // so no need to preload declaration AST.

    return Package;
  }

  // FIXME Levitation: Deprecated
  LevitationUnitInfo &createMainFilePackage(StringID MainFileID) {

    auto PackageRes = PackageInfos.insert({
      MainFileID, std::make_unique<LevitationUnitInfo>()
    });

    LevitationUnitInfo &Package = *PackageRes.first->second;

    assert(
        PackageRes.second &&
        "Only one package can be created for particular PackagePathID"
    );

    Node &DefNode = getOrCreateNode(NodeKind::Definition, MainFileID);

    DefNode.LevitationUnit = &Package;

    Package.PackagePath = MainFileID;
    Package.Declaration = nullptr;
    Package.Definition = &DefNode;
    Package.IsMainFile = true;

    return Package;
  }

  Node &getOrCreateNode(
      NodeKind Kind, StringID PackagePathID
  ) {
    auto ID = NodeID::get(Kind, PackagePathID);
    auto InsertionRes = AllNodes.insert({ ID, nullptr });

    if (InsertionRes.second)
      InsertionRes.first->second.reset(new Node(ID, Kind));

    return *InsertionRes.first->second;
  }

  void processCycles() {
    // Sometimes graph contains cycle.

    bool Success = true;

    NodesSet Visited;
    NodesSet PathNodes;
    NodesList Path;

    Log.log_trace("Checking for cycles...");

    for (auto NID : Terminals) {
      auto &trace = Log.trace();
      trace << "Checking cycles from terminal '";
      dumpNodeID(Log.trace().indent(4), NID);
      trace << "'\n";

      auto &Node = getNode(NID);

      Visited.insert(NID);

      PathNodes.insert(NID);
      Path.push_back(NID);

      Success &= processCyclesRecursive(Visited, PathNodes, Path, Node);

      Path.pop_back();
      PathNodes.erase(NID);
    }

    if (Visited.size() < AllNodes.size()) {

      Log.log_error("Found isolated nodes.");

      // Some nodes are not reachable from roots, so that means
      // they belong to isolated cycles.
      NodesSet Isolated;
      for (auto &kv : AllNodes) {
        if (!Visited.count(kv.first)) {
          Isolated.insert(kv.first);
          auto &out = Log.error();

          out.indent(4);
          dumpNodeID(out, kv.first);
          out << "\n";
        }
      }
      Invalid = true;
    }

    Invalid = !Success;
  }

  bool processCyclesRecursive(
      NodesSet &Visited,
      NodesSet &PathNodes,
      NodesList &Path,
      Node &Parent
  ) {

    bool Success = true;

    NodesSet cutSet;

    auto dumpParentChild =
    [&] (llvm::raw_ostream &out, const NodesList &Path, NodeID::Type Child) {

      out << "Existing path:\n";

      int indent = 2;
      for (auto NID : Path) {
        out.indent(indent);
        dumpNodeID(out, NID);
        out << " <--\n";
        indent += 2;
      }

      out.indent(indent) << "(new) ";
      dumpNodeID(out, Child);
      out << "\n";
    };

    NodesSet NotVisitedDependencies = Parent.Dependencies;

    while (NotVisitedDependencies.size()) {
      auto NID = *NotVisitedDependencies.begin();
      NotVisitedDependencies.erase(NID);

      auto ins = PathNodes.insert(NID);
      auto &Node = getNode(NID);

      if (!ins.second) {

        Success = false;
        Log.log_error("Found unresolvable cycle:");
        auto &out = Log.error();
        dumpParentChild(out, Path, NID);

        cutSet.insert(NID);
        Node.DependentNodes.erase(Parent.ID);
      } else {
        Path.push_back(NID);
        ins = Visited.insert(NID);
        if (ins.second)
          Success &= processCyclesRecursive(Visited, PathNodes, Path, Node);
        Path.pop_back();
      }

      PathNodes.erase(NID);
    }

    for (auto CutNID : cutSet) {
      Parent.Dependencies.erase(CutNID);
    }

    return Success;
  }

  void collectTerminals() {
    for (const auto &N : AllNodes) {
      if (N.second->DependentNodes.empty()) {
        Terminals.insert(N.first);
      }
    }
  }

  void collectPublicNodes(
      NodesSet &Visited,
      NodeID::Type ForNode,
      bool MarkPublic,
      unsigned depth
  ) {
    {
      auto _ = Log.lock();
      Log.trace().indent(depth) << "collectPublicNodes, recursive: ";
      dumpNodeID(Log.trace(), ForNode);
      Log.trace() << "\n";
    }

    auto insRes = Visited.insert(ForNode);
    if (!insRes.second && isPublic(ForNode)) {
      Log.log_trace(std::string(depth, ' '), "- skip as visited and already public.");
      return;
    }

    if (isPublic(ForNode))
      MarkPublic = true;
    else if (MarkPublic)
      PublicNodes.insert(ForNode);

    if (MarkPublic)
      Log.log_trace(std::string(depth, ' '), "- propogate public");

    const auto &N = getNode(ForNode);
    for (auto DepN : N.Dependencies)
      collectPublicNodes(Visited, DepN, MarkPublic, depth + 2);
  }

  void collectPublicNodes() {

    Log.log_verbose("Collecting public nodes...");

    NodesSet Visited;
    for (auto TerminalNID : Terminals)
      collectPublicNodes(Visited, TerminalNID, false, 0);
  }
};

}}} // end of clang::levitation::dependencies_solver namespace

#endif //LLVM_LEVITATION_DEPENDENCIES_GRAPH_H
