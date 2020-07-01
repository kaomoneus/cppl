//===--- C++ Levitation DependenciesSolver.h --------------------*- C++ -*-===//
//
// Part of the C++ Levitation Project,
// under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines C++ Levitation DependenciesSolver tool implementation.
//
//===----------------------------------------------------------------------===//

#include "clang/Levitation/Common/CreatableSingleton.h"
#include "clang/Levitation/Common/File.h"
#include "clang/Levitation/Common/FileSystem.h"
#include "clang/Levitation/Common/Path.h"
#include "clang/Levitation/Common/SimpleLogger.h"
#include "clang/Levitation/Common/StringsPool.h"
#include "clang/Levitation/Common/WithOperator.h"
#include "clang/Levitation/Dependencies.h"
#include "clang/Levitation/DependenciesSolver/DependenciesGraph.h"
#include "clang/Levitation/DependenciesSolver/SolvedDependenciesInfo.h"
#include "clang/Levitation/DependenciesSolver/DependenciesSolver.h"
#include "clang/Levitation/FileExtensions.h"
#include "clang/Levitation/Serialization.h"

#include "clang/Basic/FileManager.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <assert.h>

using namespace llvm;

namespace clang { namespace levitation { namespace dependencies_solver {

// This predeclaration is required for proper "friendship" with its
// preceeding user DependenciesSolverContext
class DependenciesSolverImpl;

namespace {

//=============================================================================
// Dependencies Solver Context

class DependenciesSolverContext {

  DependenciesSolver &Solver;

  // TODO Levitation: make it singleton?
  DependenciesStringsPool &StringsPool;

  const PathIDsSet &ExternalPackages;
  const tools::FilesMapTy &Files;


  std::shared_ptr<ParsedDependencies> ParsedDeps;
  std::shared_ptr<DependenciesGraph> DepsGraph;
  std::shared_ptr<SolvedDependenciesInfo> SolvedDepsInfo;

  friend class clang::levitation::dependencies_solver::DependenciesSolverImpl;

public:

  DependenciesSolverContext(
      DependenciesSolver &Solver,
      const PathIDsSet &externalPackages,
      const tools::FilesMapTy &files
  )
  : Solver(Solver),
    StringsPool(CreatableSingleton<DependenciesStringsPool >::get()),
    ExternalPackages(externalPackages),
    Files(files)
  {}

  DependenciesStringsPool &getStringsPool() {
    return StringsPool;
  }

  const ParsedDependencies &getParsedDependencies() const {
    assert(ParsedDeps && "ParsedDependencies should be set");
    return *ParsedDeps;
  }

  std::shared_ptr<DependenciesGraph> &&detachDependenciesGraph() {
    assert(DepsGraph && "DependenciesGraph should be set");
    return std::move(DepsGraph);
  }

  // TODO Levitation: probably pick up better name.
  std::shared_ptr<SolvedDependenciesInfo> detachSolvedDependenciesInfo() {
    assert(SolvedDepsInfo && "SolvedDependenciesInfo should be set");
    return std::move(SolvedDepsInfo);
  }

  // TODO Levitation: Deprecated
  const SolvedDependenciesInfo &getSolvedDependenciesInfo() const {
    assert(SolvedDepsInfo && "SolvedDependenciesInfo should be set");
    return *SolvedDepsInfo;
  }

  const PathIDsSet &getExternalPackages() const {
    return ExternalPackages;
  }
};

} // end of anonymous namespace

using SolvedDependenciesMap = llvm::DenseMap<llvm::StringRef, std::unique_ptr<SolvedDependenciesInfo>>;

class DependenciesSolverImpl {
  DependenciesSolverContext &Context;
  log::Logger &Log = log::Logger::get();
  DependenciesSolver *Solver;

public:
  DependenciesSolverImpl(
      DependenciesSolverContext &Context
  ) : Context(Context), Solver(&Context.Solver) {}

  DependenciesSolverContext &getContext() { return Context; }

  static void collectFilesWithExtension(
      Paths &Dest,
      Paths &NewSubDirs,
      llvm::vfs::FileSystem &FS,
      StringRef CurDir,
      StringRef FileExtension
  ) {
    std::error_code EC;

    for (
      llvm::vfs::directory_iterator Dir = FS.dir_begin(CurDir, EC), e;
      Dir != e && !EC;
      Dir.increment(EC)
    ) {
      StringRef Path = Dir->path();

      switch (Dir->type()) {
        case llvm::sys::fs::file_type::regular_file:
          if (llvm::sys::path::extension(Path) == FileExtension)
            Dest.push_back(Path);
        break;

        case llvm::sys::fs::file_type::directory_file:
          NewSubDirs.push_back(Path);
        break;

        default:
        break;
      }
    }
  }

  // TODO Levitation: Deprecated
  static Paths collectLDepsFiles(llvm::StringRef BuildRoot) {

    Paths LDepsFiles;
    auto &Log = log::Logger::get();

    Log.log_verbose("Collecting dependencies...");

    auto &FM = CreatableSingleton<FileManager>::get();
    auto &FS = FM.getVirtualFileSystem();

    Paths SubDirs;
    SubDirs.push_back(BuildRoot);

    std::string parsedDepsFileExtension = ".";
    parsedDepsFileExtension += FileExtensions::ParsedDependencies;

    Paths NewSubDirs;
    while (SubDirs.size()) {
      NewSubDirs.clear();
      for (StringRef CurDir : SubDirs) {
        collectFilesWithExtension(
            LDepsFiles,
            NewSubDirs,
            FS,
            CurDir,
            parsedDepsFileExtension
        );
      }
      SubDirs.swap(NewSubDirs);
    }

    Log.log_verbose(
      "Found ", LDepsFiles.size(),
      " '.", FileExtensions::ParsedDependencies,
      "' files.\n" // it was meant to put extra new line here.
    );

    return LDepsFiles;
  }

  void collectParsedDependencies() {
    loadDependencies(Context.Files);

    with (auto verb = Log.acquire(log::Level::Verbose)) {
      auto &s = verb.s;
      s << "Loaded dependencies:\n";
      dump(s, Context.getParsedDependencies());
    }
  }

  bool sourceExists(StringRef Source) {

    if (llvm::sys::path::is_absolute(Source))
      return llvm::sys::fs::exists(Source);

    PathString OriginalSourceFull = Context.Solver.SourcesRoot;
    llvm::sys::path::append(OriginalSourceFull, Source);

    return llvm::sys::fs::exists(OriginalSourceFull);
  }

  bool loadFromBuffer(
      StringID PackageID,
      ParsedDependencies &Dest,
      const llvm::MemoryBuffer &MemBuf
  ) {
    auto &Log = log::Logger::get();

    auto Reader = CreateBitstreamReader(MemBuf);

    DependenciesData PackageData;

    if (!Reader->read(PackageData)) {
      Log.log_error(Reader->getStatus().getErrorMessage());
      return false;
    }

    if (Reader->getStatus().hasWarnings()) {
      Log.log_warning(Reader->getStatus().getWarningMessage());
    }

    Dest.add(PackageID, PackageData);

    return true;
  }

  void loadDependencies(const tools::FilesMapTy &ParsedDepFiles) {
    Context.ParsedDeps = std::make_shared<ParsedDependencies>(
        Context.getStringsPool()
    );
    ParsedDependencies &Dest = *Context.ParsedDeps;

    Log.log_verbose("Loading dependencies info...");

    auto &FM = CreatableSingleton<FileManager>::get();

    for (auto &kv : ParsedDepFiles.getUniquePtrMap()) {
      StringID PackageID = kv.first;
      StringRef LDepPath = kv.second->LDeps;

      if (auto Buffer = FM.getBufferForFile(LDepPath)) {

        Log.log_trace("  Reading '", LDepPath, "'...");

        llvm::MemoryBuffer &MemBuf = *Buffer.get();

        if (!loadFromBuffer(PackageID, Dest, MemBuf)) {
          Solver->setFailure("Failed to read dependencies");
          Log.log_error("Failed to read dependencies for '", LDepPath, "'");
        }
      } else {
       Solver->setFailure("Failed to open one of dependency files");
       Log.log_error("Failed to open file '", LDepPath, "'\n");
      }
    }

    // Check missed dependencies

    bool FoundMissedDependencies = false;

    auto checkSource = [&] (StringID PackageID, StringRef CheckName, StringRef SourceType) {
      const auto &PackageStr = *Context.getStringsPool().getItem(PackageID);

      auto *PackageFile = Context.Files.tryGet(PackageID);

      Log.log_trace(CheckName, " '", PackageStr, "'...");

      if (!PackageFile) {
        Log.log_error(
            "Missed ", SourceType,
            " '", PackageStr, "'"
        );
        return false;
      }

      if (!sourceExists(PackageFile->Source)) {
        Log.log_error(
            "Missed ", SourceType, " '",
            PackageStr,
            "' -> '",
            PackageFile, "' : it was found, but then disappeared."
        );
        llvm_unreachable("Fatal unexpected source kidnapping.");
      }
      return true;
    };

    for (const auto &kv : Dest) {

      if (!checkSource(kv.first, "Checking package", "package")) {
        FoundMissedDependencies = true;
      } else {
        for (const auto &Dep : kv.second->DeclarationDependencies) {
          FoundMissedDependencies |= !checkSource(
              Dep.UnitIdentifier,
              "  -- checking decl dep",
              "declaration dependency"
          );
        }
        for (const auto &Dep : kv.second->DefinitionDependencies) {
          FoundMissedDependencies |= !checkSource(
              Dep.UnitIdentifier,
              "  -- checking def dep",
              "definition dependency"
          );
        }
      }
    }

    if (FoundMissedDependencies) {
      Log.log_error("Can't continue due to missed dependencies.");
      Solver->setFailure("Can't continue due to missed dependencies.");
    }
  }

  static void dump(llvm::raw_ostream &out, const ParsedDependencies &ParsedDependencies) {
    for (auto &PackageDependencies : ParsedDependencies) {
      StringID PackageID = PackageDependencies.first;
      DependenciesData &Data = *PackageDependencies.second;

      auto &Strings = *Data.Strings;

      out
      << "Package #" << PackageID
      << ": " << *Strings.getItem(PackageID) << "\n";

      if (
        Data.DeclarationDependencies.empty() &&
        Data.DefinitionDependencies.empty()
      ) {
        out
        << "    no dependencies.\n";
      } else {
        dump(out, 4, "Declaration depends on:", Strings, Data.DeclarationDependencies);
        dump(out, 4, "Definition depends on:", Strings, Data.DefinitionDependencies);
      }

      out << "\n";
    }
  }

  void buildDependenciesGraph() {
    if (!Solver->isValid())
      return;

    auto DGraph = DependenciesGraph::build(
        Context.getParsedDependencies(),
        Context.getExternalPackages()
    );

    Context.DepsGraph = DGraph;

    if (DGraph->isInvalid()) {
      with(auto err = Log.acquire(log::Level::Error)) {
        auto &s = err.s;
        s << "Failed to solve dependencies. Unable to find root nodes.\n";
        if (!Solver->Verbose) {
          s << "Loaded dependencies:\n";
          dump(s, Context.getParsedDependencies());
        }
        return;
      }
    }

    with (auto verb = Log.acquire(log::Level::Verbose)) {
      auto &s = verb.s;
      s << "Dependencies graph:\n";
      DGraph->dump(s, Context.StringsPool);
    }
  }

  void solveGraph() {
    if (!Solver->isValid())
      return;

    std::shared_ptr<DependenciesGraph> DGraphPtr =
        Context.detachDependenciesGraph();

    // TODO Levitation: remove this way. Use singleton instead.
    auto &Strings = Context.getStringsPool();

      Log.log_verbose("Solving dependencies...");

      Context.SolvedDepsInfo = SolvedDependenciesInfo::build(DGraphPtr);

      const auto &SolvedInfo = *Context.SolvedDepsInfo;

      if (!SolvedInfo.isValid()) {
        with(auto err = Log.acquire(log::Level::Error)) {
          auto &s = err.s;
          s << "Failed to solve: " << SolvedInfo.getErrorMessage() << "\n";
          s << "Dependencies:\n";
          dump(s, Context.getParsedDependencies());
        }

        Solver->setFailure("Failed to solve dependencies.");
        return;
      }

      with(auto verb = Log.acquire(log::Level::Verbose)) {
        auto &s = verb.s;
        s << "Dependencies solved info:\n";
        SolvedInfo.dump(s, Strings);
      }
  }

  static void dump(
      llvm::raw_ostream &out,
      unsigned Indent,
      StringRef Title,
      DependenciesStringsPool &Strings,
      DependenciesData::DeclarationsBlock &Deps
  ) {
    if (Deps.empty())
      return;

    out.indent(Indent) << Title << "\n";
    for (auto &Dep : Deps) {
        out.indent(Indent + 4)
        << *Strings.getItem(Dep.UnitIdentifier) << "\n";
    }
  }

  using PathString = SinglePath;
  using DependenciesPaths = Paths;

  PathString getNodeSourceFilePath(
      const StringRef ParentDir,
      const DependenciesStringsPool &Strings,
      const DependenciesGraph::Node &Node
  ) {
    PathString SourcePath = *Strings.getItem(Node.LevitationUnit->UnitPath);

    StringRef NewExt = FileExtensions::SourceCode;

    updateFilePath(SourcePath, ParentDir, NewExt);

    return SourcePath;
  }

  PathString getNodeFilePath(
      const StringRef ParentDir,
      const DependenciesStringsPool &Strings,
      const DependenciesGraph::Node &Node
  ) {
    PathString SourcePath = *Strings.getItem(Node.LevitationUnit->UnitPath);

    StringRef NewExt = Node.Kind == DependenciesGraph::NodeKind::Declaration ?
        FileExtensions::DeclarationAST :
        FileExtensions::Object;

    updateFilePath(SourcePath, ParentDir, NewExt);

    return SourcePath;
  }

  void updateFilePath(
      PathString &SourcePath,
      const StringRef ParentDir,
      StringRef NewExt
  ) {
    if (SourcePath.startswith("./"))
      SourcePath = SourcePath.substr(2);

    llvm::sys::path::replace_extension(SourcePath, NewExt);
    llvm::sys::fs::make_absolute(ParentDir, SourcePath);

    if (SourcePath.startswith("./"))
      SourcePath = SourcePath.substr(2);

    llvm::sys::fs::make_absolute(SourcePath);
  }

  // TODO Levitation: Deprecated
  DependenciesPaths buildDirectDependencies(
    StringRef DepsRoot,
    const DependenciesStringsPool &Strings,
    const DependenciesGraph &DGraph,
    const DependenciesGraph::NodesSet &Dependencies
  ) {
    DependenciesPaths Paths;
    for (auto NID : Dependencies) {
      assert(
          DependenciesGraph::NodeID::getKind(NID) ==
          DependenciesGraph::NodeKind::Declaration &&
          "Only declaration nodes are allowed to be dependencies"
      );

      const auto &N = DGraph.getNode(NID);

      // Also put source file into dependencies
      // Because we need to rebuild target in case
      // if source file deleted.
      auto NodeSourceFilePath = getNodeSourceFilePath(
          Context.Solver.SourcesRoot,
          Strings,
          N
      );
      Paths.push_back(NodeSourceFilePath);

      auto NodeFilePath = getNodeFilePath(
          DepsRoot, Strings, DGraph.getNode(NID)
      );
      Paths.push_back(std::move(NodeFilePath));
    }

    return Paths;
  }

  // TODO Levitation: Deprecated
  bool writeDependenciesFile(
      DependenciesSolver &Solver,
      StringRef Extension,
      StringRef DependentFilePath,
      const DependenciesPaths &Dependencies
  ) {
    auto DependenciesFile = (DependentFilePath + "." + Extension).str();

    Log.log_verbose("Writing '", DependenciesFile, "'...");

    File F(DependenciesFile);

    if (F.hasErrors()) {
      Solver.setFailure()
      << "failed to write file '" << DependenciesFile << "'";
      return false;
    }

    with (auto Opened = F.open()) {
      auto &out = Opened.getOutputStream();

      for (auto D : Dependencies)
        out << D << "\n";
    }

    if (F.hasErrors()) {
      Solver.setFailure()
      << "failed to complete file '" << DependenciesFile << "'";
      return false;
    }

    return true;
  }

  void solve() {
    collectParsedDependencies();
    buildDependenciesGraph();
    solveGraph();
  }
};

std::shared_ptr<SolvedDependenciesInfo>
DependenciesSolver::solve(
    const PathIDsSet &ExternalPackages,
    const tools::FilesMapTy &Files
) {
  auto &Log = log::Logger::get();

  DependenciesSolverContext Context(*this, ExternalPackages, Files);
  DependenciesSolverImpl Impl(Context);

  Impl.solve();

  if (isValid()) {
    Log.log_verbose("\nComplete");

    return Context.detachSolvedDependenciesInfo();
  }

  Log.log_error(getErrorMessage());
  return nullptr;
}

}}}
