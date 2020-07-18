//===--- C++ Levitation Driver.cpp ------------------------------*- C++ -*-===//
//
// Part of the C++ Levitation Project,
// under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file contains implementation for C++ Levitation Driver methods
//
//===----------------------------------------------------------------------===//

#include "clang/Basic/FileManager.h"
#include "clang/Config/config.h"

#include "clang/Levitation/Common/Failable.h"
#include "clang/Levitation/Common/File.h"
#include "clang/Levitation/Common/FileSystem.h"
#include "clang/Levitation/Common/SimpleLogger.h"
#include "clang/Levitation/Common/StringBuilder.h"
#include "clang/Levitation/DeclASTMeta/DeclASTMeta.h"
#include "clang/Levitation/DeclASTMeta/DeclASTMetaLoader.h"
#include "clang/Levitation/DependenciesSolver/DependenciesGraph.h"
#include "clang/Levitation/DependenciesSolver/DependenciesSolver.h"
#include "clang/Levitation/DependenciesSolver/SolvedDependenciesInfo.h"
#include "clang/Levitation/Driver/Driver.h"
#include "clang/Levitation/Driver/PackageFiles.h"
#include "clang/Levitation/Driver/HeaderGenerator.h"
#include "clang/Levitation/FileExtensions.h"
#include "clang/Levitation/TasksManager/TasksManager.h"
#include "clang/Levitation/UnitID.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/ADT/None.h"

#include <algorithm>
#include <memory>
#include <system_error>
#include <utility>

namespace clang { namespace levitation { namespace tools {

using namespace clang::levitation::dependencies_solver;
using namespace clang::levitation::tasks;

//-----------------------------------------------------------------------------
//  Levitation driver implementation classes
//  (LevitationDriver itself is defined at the bottom)

namespace {

  // TODO Levitation: Whole Context approach is malformed.
  // Context should keep shared data for all sequence steps.
  // If something is required for particular step only it should
  // be out of context.

  struct RunContext {

    // TODO Levitation: Introduce LevitationDriverOpts and use here
    // its reference instead.
    LevitationDriver &Driver;
    Failable Status;

    PathIDsSet AllPackages;
    PathIDsSet ProjectPackages;
    PathIDsSet ExternalPackages;

    FilesMapTy Files;

    std::shared_ptr<SolvedDependenciesInfo> DependenciesInfo;

    bool PreambleUpdated = false;
    bool ObjectsUpdated = false;
    DependenciesGraph::NodesSet UpdatedNodes;

    RunContext(LevitationDriver &driver)
    : Driver(driver)
    {}
  };
}

class LevitationDriverImpl {
  RunContext &Context;
  DependenciesStringsPool &Strings;

  Failable &Status;
  log::Logger &Log;
  TasksManager &TM;

public:

  explicit LevitationDriverImpl(RunContext &context)
  : Context(context),
    Strings(CreatableSingleton<DependenciesStringsPool>::get()),
    Status(context.Status),
    Log(log::Logger::get()),
    TM(TasksManager::get())
  {}

  void buildPreamble();

  // TODO Levitation: Deprecated
  void runParse();
  void runParseImport();
  void solveDependencies();

  // TODO Levitation: Deprecated
  void instantiateAndCodeGen();

  void codeGen();
  void runLinker();

  void collectSources();

private:

  void collectProjectSources();
  void collectLibrariesSources();

  void setOutputFilesInfo(
      FilesInfo& Info,
      StringRef OutputPathWithoutExt,
      bool SetObjectRelatedInfo
  );

  // TODO Levitation: deprecated
  void addMainFileInfo();

  // TODO Levitation: deprecated
  bool processDependencyNodeDeprecated(
      const DependenciesGraph::Node &N
  );

  /// Dependency node processing
  /// \param N node to be processed
  /// \return true is successful
  bool processDependencyNode(
      const DependenciesGraph::Node &N
  );

  bool processDefinition(const DependenciesGraph::Node &N);

  bool processDeclaration(
      HashRef ExistingMeta,
      const DependenciesGraph::Node &N
  );

  bool isUpToDate(DeclASTMeta &Meta, const DependenciesGraph::Node &N);

  bool isUpToDate(
    DeclASTMeta &Meta,
    StringRef ProductFile,
    StringRef MetaFile,
    StringRef SourceFile,
    StringRef ItemDescr
  );

  void setPreambleUpdated();
  void setNodeUpdated(DependenciesGraph::NodeID::Type NID);
  void setObjectsUpdated();

  const FilesInfo& getFilesInfoFor(
      const DependenciesGraph::Node &N
  ) const;

  Paths getFullDependencies(
      const DependenciesGraph::Node &N,
      const DependenciesGraph &Graph
  ) const;

  Paths getIncludeSources(
      const DependenciesGraph::Node &N,
      const DependenciesGraph &Graph
  ) const;

  Paths getImportSources(
      const DependenciesGraph::Node &N,
      const DependenciesGraph &Graph
  ) const;
};

/*static*/
class ArgsUtils {

  enum class QuoteType {
      None,
      SingleQuote,
      DoubleQuote
  };

  class ArgsBuilder {
    StringRef ArgsString;
    LevitationDriver::Args Args;

    std::size_t ArgStart = 0;
    std::size_t ArgEnd = 0;
    llvm::SmallVector<size_t, 16> ArgEscapes;

    QuoteType QuoteOpened = QuoteType::None;

    bool EscapeOn = false;

    bool isCurArgEmpty() const {
      return ArgStart == ArgEnd;
    }

    bool isQuoteOpened() const {
      return QuoteOpened != QuoteType::None;
    }

    void newStartPos() {
      ++ArgEnd;
      ArgStart = ArgEnd;
    }

    void addSymbol() {
      ++ArgEnd;
    }

    void skipSymbolAsEscape() {
      auto CurSymbol = ArgEnd++;
      ArgEscapes.push_back(CurSymbol);
    }

    void commitArg() {

      if (isCurArgEmpty()) {
        newStartPos();
        return;
      }

      if (ArgEscapes.empty()) {
        auto Arg = ArgsString.substr(ArgStart, ArgEnd - ArgStart);
        Args.push_back(Arg);
        return;
      }

      levitation::StringBuilder sb;
      sb << ArgsString.substr(ArgStart, ArgEscapes[0] - ArgStart);
      size_t e = ArgEscapes.size();

      for (size_t i = 1; i != e; ++i) {
        size_t Start = ArgEscapes[i-1] + 1;
        size_t End = ArgEscapes[i];
        sb << ArgsString.substr(Start, End - Start);
      }

      size_t Start = ArgEscapes[e-1] + 1;
      size_t End = ArgEnd;
      sb << ArgsString.substr(Start, End - Start);

      Args.emplace_back(std::move(sb.str()));
    }

  public:

    ArgsBuilder(StringRef argsString)
    : ArgsString(argsString)
    {}

    ~ArgsBuilder() {}

    void onQuote(QuoteType quoteType) {
      if (!isQuoteOpened()) {

        QuoteOpened = quoteType;
        onRegularSymbol();
        return;

      } if (QuoteOpened == quoteType) {
        QuoteOpened = QuoteType::None;

        // In case if quote symbol will turned out to be last
        // one before space, it will be truncated
        addSymbol();
      }
    }

    void onEscape() {
      if (EscapeOn) {
        addSymbol();
        EscapeOn = false;
        return;
      }

      EscapeOn = true;
      skipSymbolAsEscape();
    }

    void onRegularSymbol() {
      addSymbol();
    }

    void onSpace() {
      if (QuoteOpened != QuoteType::None || EscapeOn) {
        addSymbol();
        return;
      }

      commitArg();

      newStartPos();
    }

    void detachArgsTo(LevitationDriver::Args &Dest) {
      commitArg();
      Dest.swap(Args);
    }
  };

  static StringRef stripBoundingQuotesIfPresent(StringRef S) {
    size_t e = S.size();
    if (e < 2)
      return S;

    if (S[0] == S[e-1] && (S[0] == '\'' || S[0] == '"'))
      return S.substr(1, e-2);

    return S;
  }

public:
  static LevitationDriver::Args parse(StringRef S) {
    ArgsBuilder Builder(S);

    for (unsigned i = 0, e = S.size(); i != e; ++i) {
      char Symbol = S[i];

      switch (Symbol) {
        case '"':
          Builder.onQuote(QuoteType::DoubleQuote);
          break;

        case '\'':
          Builder.onQuote(QuoteType::SingleQuote);
          break;

        case '\\':
          Builder.onEscape();
          break;

        case ' ':
          Builder.onSpace();
          break;

        default:
          Builder.onRegularSymbol();
      }
    }

    LevitationDriver::Args Res;
    Builder.detachArgsTo(Res);
    return Res;
  }

  static SmallVector<StringRef, 16> toStringRefArgs(
      const LevitationDriver::Args &InputArgs
  ) {
    SmallVector<StringRef, 16> Args;
    Args.reserve(InputArgs.size());

    for (const auto &A : InputArgs) {
      auto AStripped = stripBoundingQuotesIfPresent(A);
      Args.push_back(AStripped);
    }

    return Args;
  }

  static void dump(
      llvm::raw_ostream& Out,
      const LevitationDriver::Args &Args
  ) {
    if (Args.empty())
      return;

    for (unsigned i = 0, e = Args.size(); i != e; ++i) {
      if (i != 0)
        Out << " ";
      Out << Args[i];
    }
  }
};

/*static*/
class Commands {
public:
  class CommandInfo {
    using Args = LevitationDriver::Args;
    llvm::SmallVector<SmallString<256>, 8> OwnArgs;
    SinglePath ExecutablePath;
    Args CommandArgs;
    log::Logger &Log;

    bool Condition = true;
    bool Verbose;
    bool DryRun;

    CommandInfo(
        SinglePath &&executablePath,
        bool verbose,
        bool dryRun
    )
    : ExecutablePath(std::move(executablePath)),
      Log(log::Logger::get()),
      Verbose(verbose),
      DryRun(dryRun)
    {
      CommandArgs.push_back(ExecutablePath);
    }

public:
    CommandInfo() = delete;

    StringRef getExecutablePath() const {
      return ExecutablePath;
    }

    const Args &getCommandArgs() const {
      return CommandArgs;
    }

    static CommandInfo getBuildPreamble(
        StringRef BinDir,
        const SmallVectorImpl<SinglePath> &Includes,
        StringRef StdLib,
        bool verbose,
        bool dryRun
    ) {
      auto Cmd = getClangXXCommand(BinDir, Includes, StdLib, verbose, dryRun);

      Cmd
      .addArg("-cppl-preamble");

      return Cmd;
    }

    static CommandInfo getParseImport(
        StringRef BinDir,
        StringRef PrecompiledPreamble,
        bool verbose,
        bool dryRun
    ) {

      auto Cmd = getClangXXCommandBase(
          BinDir, "", verbose, dryRun
      );

      Cmd.addArg("-cppl-import");

      if (PrecompiledPreamble.size())
        Cmd.addKVArgEq("-cppl-include-preamble", PrecompiledPreamble);

      return Cmd;
    }

    static CommandInfo getBuildDecl(
        StringRef BinDir,
        const SmallVectorImpl<SinglePath> &Includes,
        StringRef PrecompiledPreamble,
        StringRef StdLib,
        bool verbose,
        bool dryRun
    ) {
      auto Cmd = getClangXXCommand(BinDir, Includes, StdLib, verbose, dryRun);

      Cmd
      .addArg("-xc++")
      .addArg("-cppl-decl");
      return Cmd;
    }

    static CommandInfo getBuildObj(
        StringRef BinDir,
        const SmallVectorImpl<SinglePath> &Includes,
        StringRef PrecompiledPreamble,
        StringRef StdLib,
        bool verbose,
        bool dryRun
    ) {
      auto Cmd = getClangXXCommand(BinDir, Includes, StdLib, verbose, dryRun);
      Cmd
      .addArg("-cppl-obj");
      return Cmd;
    }

    static CommandInfo getLink(
        StringRef BinDir,
        StringRef StdLib,
        bool verbose,
        bool dryRun,
        bool CanUseLibStdCpp
    ) {
      CommandInfo Cmd(getClangXXPath(BinDir), verbose, dryRun);

      if (CanUseLibStdCpp)
        Cmd.addKVArgEqIfNotEmpty("-stdlib", StdLib);

#ifdef LEVITATION_DEFAULT_LINKER_VERSION
      Cmd.addKVArgEqIfNotEmpty("-mlinker-version", LEVITATION_DEFAULT_LINKER_VERSION);
#endif

      return Cmd;
    }

    CommandInfo& addArg(StringRef Arg) {
      if (!Condition) return *this;
      CommandArgs.emplace_back(Arg);
      return *this;
    }

    CommandInfo& addKVArgSpace(StringRef Arg, StringRef Value) {
      if (!Condition) return *this;
      CommandArgs.emplace_back(Arg);
      CommandArgs.emplace_back(Value);
      return *this;
    }

    CommandInfo& addKVArgEq(StringRef Arg, StringRef Value) {
      if (!Condition) return *this;
      OwnArgs.emplace_back((Arg + "=" + Value).str());
      CommandArgs.emplace_back(OwnArgs.back());
      return *this;
    }

    CommandInfo& addKVArgEqIfNotEmpty(StringRef Arg, StringRef Value) {
      if (!Condition) return *this;
      if (Value.size())
        addKVArgEq(Arg, Value);
      return *this;
    }

    template <typename ValuesT>
    CommandInfo& addArgs(const ValuesT& Values) {
      if (!Condition) return *this;
      for (const auto &Value : Values) {
        CommandArgs.emplace_back(Value);
      }
      return *this;
    }

    template <typename ValuesT>
    CommandInfo& addKVArgsEq(StringRef Name, const ValuesT& Values) {
      if (!Condition) return *this;
      for (const auto &Value : Values) {
        OwnArgs.emplace_back((Name + "=" + Value).str());
        CommandArgs.emplace_back(OwnArgs.back());
      }
      return *this;
    }

    template <typename ValuesT>
    CommandInfo& addKVArgsSpace(
        StringRef Name,
        const ValuesT& Values,
        bool addQuotes = false
    ) {
      if (!Condition) return *this;

      for (const auto &Value : Values) {

        StringBuilder sb;
        sb << Name << " ";
        if (addQuotes)
          sb << "\"" << Value << "\"";
        else
          sb << Value;

        OwnArgs.emplace_back(std::move(sb.str()));
        CommandArgs.emplace_back(OwnArgs.back());
      }
      return *this;
    }

    CommandInfo& condition(bool Value) {
      Condition = Value;
      return *this;
    }

    CommandInfo& conditionElse() {
      Condition = !Condition;
      return *this;
    }

    CommandInfo& conditionEnd() {
      Condition = true;
      return *this;
    }

    Failable execute() {
      if (DryRun || Verbose) {
        dumpCommand();
      }

      if (!DryRun) {
        std::string ErrorMessage;

        auto Args = ArgsUtils::toStringRefArgs(CommandArgs);

        unsigned ExecJobID = getExecID();
        Log.log_trace("Trying to execute exec job ID=", ExecJobID);

        int Res = llvm::sys::ExecuteAndWait(
            ExecutablePath,
            Args,
            /*Env*/llvm::None,
            /*Redirects*/{},
            /*secondsToWait*/ 0,
            /*memoryLimit*/ 0,
            &ErrorMessage,
            /*ExectutionFailed*/nullptr
        );

        Failable Status;

        if (Res != 0) {
          Status.setFailure() << ErrorMessage;
        } else if (ErrorMessage.size()) {
          Status.setWarning() << ErrorMessage;
        }

        Log.log_trace("Result for exec job ID=", ExecJobID, " is ", Res);

        return Status;
      }

      return Failable();
    }
  protected:

    static SinglePath getClangPath(llvm::StringRef BinDir) {

      const char *ClangBin = "clang";

      if (BinDir.size()) {
        SinglePath P = BinDir;
        llvm::sys::path::append(P, ClangBin);
        return P;
      }

      return SinglePath(ClangBin);
    }

    static SinglePath getClangXXPath(llvm::StringRef BinDir) {

      const char *ClangBin = "clang++";

      if (BinDir.size()) {
        SinglePath P = BinDir;
        llvm::sys::path::append(P, ClangBin);
        return P;
      }

      return SinglePath(ClangBin);
    }

    static CommandInfo getClangXXCommand(
        llvm::StringRef BinDir,
        const SmallVectorImpl<SinglePath> &Includes,
        StringRef StdLib,
        bool verbose,
        bool dryRun
    ) {
      CommandInfo Cmd = getClangXXCommandBase(BinDir, StdLib, verbose, dryRun);
      Cmd.addKVArgsSpace("-I", Includes, /*addQuotes*/ true);
      return Cmd;
    }

    static CommandInfo getClangXXCommandBase(
        llvm::StringRef BinDir,
        StringRef StdLib,
        bool verbose,
        bool dryRun
    ) {
      CommandInfo Cmd(getClangXXPath(BinDir), verbose, dryRun);
      Cmd
      .addArg("-std=c++17")
      .addKVArgEqIfNotEmpty("-stdlib", StdLib);

      return Cmd;
    }

    static CommandInfo getBase(
        llvm::StringRef BinDir,
        llvm::StringRef PrecompiledPreamble,
        bool verbose,
        bool dryRun
    ) {
      CommandInfo Cmd(getClangPath(BinDir), verbose, dryRun);
      Cmd.setupCCFlags();

      if (PrecompiledPreamble.size())
        Cmd.addKVArgEq("-levitation-preamble", PrecompiledPreamble);

      return Cmd;
    }

    void setupCCFlags() {
       addArg("-cc1")
      .addArg("-std=c++17")
      .addArg("-stdlib=libstdc++");
    }

    static unsigned getExecID() {
      static std::mutex Locker;
      static unsigned NextExecID = 0;

      {
        MutexLock _(Locker);
        return NextExecID++;
      }
    }

    void dumpCommand() {
      with (auto info = Log.acquire(log::Level::Info)) {
        auto &Out = info.s;

        for (unsigned i = 0, e = CommandArgs.size(); i != e; ++i) {
          if (i != 0)
            Out << " ";
          Out << CommandArgs[i];
        }

        Out << "\n";
      }
    }
  };

  static bool parseImport(
      StringRef BinDir,
      StringRef PrecompiledPreamble,
      StringRef OutLDepsFile,
      StringRef OutLDepsMetaFile,
      StringRef SourceFile,
      StringRef SourcesRoot,
      const LevitationDriver::Args &ExtraArgs,
      bool Verbose,
      bool DryRun
  ) {
    if (!DryRun || Verbose)
      dumpParseImport(OutLDepsFile, SourceFile);

    levitation::Path::createDirsForFile(OutLDepsFile);

    auto ExecutionStatus = CommandInfo::getParseImport(
        BinDir, PrecompiledPreamble, Verbose, DryRun
    )
    .addKVArgEq("-cppl-src-root", SourcesRoot)
    .addKVArgEq("-cppl-deps-out", OutLDepsFile)
    .addKVArgEq("-cppl-meta", OutLDepsMetaFile)
    .addArgs(ExtraArgs)
    .addArg(SourceFile)
    .execute();

    return processStatus(ExecutionStatus);
  }

  static bool buildDecl(
      StringRef BinDir,
      const SmallVectorImpl<SinglePath> &Includes,
      StringRef PrecompiledPreamble,
      StringRef OutDeclASTFile,
      StringRef OutDeflASTMetaFile,
      StringRef InputFile,
      StringRef UnitID,
      const Paths &Deps,
      StringRef StdLib,
      const LevitationDriver::Args &ExtraParserArgs,
      bool Verbose,
      bool DryRun
  ) {
    assert(OutDeclASTFile.size() && InputFile.size());

    if (!DryRun || Verbose)
      dumpBuildDecl(OutDeclASTFile, OutDeflASTMetaFile, InputFile, Deps);

    levitation::Path::createDirsForFile(OutDeclASTFile);

    auto ExecutionStatus = CommandInfo::getBuildDecl(
        BinDir, Includes, PrecompiledPreamble, StdLib, Verbose, DryRun
    )
    .addKVArgEqIfNotEmpty("-cppl-include-preamble", PrecompiledPreamble)
    .addKVArgsEq("-cppl-include-dependency", Deps)
    .addArgs(ExtraParserArgs)
    .addArg(InputFile)
    .addKVArgEq("-cppl-unit-id", UnitID)
    // TODO Levitation: don't emit .decl-ast files
    //  in some cases. See task #48
    //    .condition(OutDeclASTFile.size())
    //        .addKVArgSpace("-o", OutDeclASTFile)
    //    .conditionElse()
    //        .addArg("-cppl-no-out")
    //    .conditionEnd()
    .addKVArgSpace("-o", OutDeclASTFile)
    .addKVArgEq("-cppl-meta", OutDeflASTMetaFile)
    .execute();

    return processStatus(ExecutionStatus);
  }

  static bool buildObject(
      StringRef BinDir,
      const SmallVectorImpl<SinglePath>& Includes,
      StringRef PrecompiledPreamble,
      StringRef OutObjFile,
      StringRef OutMetaFile,
      StringRef InputObject,
      StringRef UnitID,
      const Paths &Deps,
      StringRef StdLib,
      const LevitationDriver::Args &ExtraParserArgs,
      const LevitationDriver::Args &ExtraCodeGenArgs,
      bool Verbose,
      bool DryRun
  ) {
    assert(OutObjFile.size() && InputObject.size());

    if (!DryRun || Verbose)
      dumpBuildObject(OutObjFile, InputObject, Deps);

    levitation::Path::createDirsForFile(OutObjFile);

    auto ExecutionStatus = CommandInfo::getBuildObj(
        BinDir, Includes, PrecompiledPreamble, StdLib, Verbose, DryRun
    )
    .addKVArgEqIfNotEmpty("-cppl-include-preamble", PrecompiledPreamble)
    .addKVArgsEq("-cppl-include-dependency", Deps)
    .addArgs(ExtraParserArgs)
    .addArgs(ExtraCodeGenArgs)
    .addArg(InputObject)
    .addKVArgEq("-cppl-unit-id", UnitID)
    .addKVArgSpace("-o", OutObjFile)
    .addKVArgEq("-cppl-meta", OutMetaFile)
    .execute();

    return processStatus(ExecutionStatus);
  }

  static bool buildPreamble(
      StringRef BinDir,
      const SmallVectorImpl<SinglePath>& Includes,
      StringRef PreambleSource,
      StringRef PCHOutput,
      StringRef PCHOutputMeta,
      StringRef StdLib,
      const LevitationDriver::Args &ExtraPreambleArgs,
      bool Verbose,
      bool DryRun
  ) {
    assert(PreambleSource.size() && PCHOutput.size());

    if (!DryRun || Verbose)
      dumpBuildPreamble(PreambleSource, PCHOutput);

    levitation::Path::createDirsForFile(PCHOutput);

    auto ExecutionStatus = CommandInfo::getBuildPreamble(
        BinDir, Includes, StdLib, Verbose, DryRun
    )
    .addArg(PreambleSource)
    .addKVArgSpace("-o", PCHOutput)
    .addKVArgEq("-cppl-meta", PCHOutputMeta)
    .addArgs(ExtraPreambleArgs)
    .execute();

    return processStatus(ExecutionStatus);
  }

  static bool link(
      StringRef BinDir,
      StringRef OutputFile,
      const Paths &ObjectFiles,
      StringRef StdLib,
      const LevitationDriver::Args &ExtraArgs,
      bool Verbose,
      bool DryRun,
      bool CanUseLibStdCpp
  ) {
    assert(OutputFile.size() && ObjectFiles.size());

    if (!DryRun || Verbose)
      dumpLink(OutputFile, ObjectFiles);

    levitation::Path::createDirsForFile(OutputFile);

    auto ExecutionStatus = CommandInfo::getLink(
        BinDir, StdLib, Verbose, DryRun, CanUseLibStdCpp
    )
    .addArgs(ExtraArgs)
    .addArgs(ObjectFiles)
    .addKVArgSpace("-o", OutputFile)
    .execute();

    return true;
  }

protected:

  static log::manipulator_t workerId() {
    auto &TM = TasksManager::get();
    auto WID = TM.getWorkerID();
      if (TasksManager::isValid(WID))
        return [=] (llvm::raw_ostream &out) {
          out << WID;
        };
      else
        return [=] (llvm::raw_ostream &out) {
          out << "Main";
        };
  }

  template <typename ...ArgsT>
  static void log_info(ArgsT&&...args) {
    log::Logger::get().log_info(
        "[", workerId(), "] ",
        std::forward<ArgsT>(args)...
    );
  }

  template <typename ...ArgsT>
  static void log_verbose(ArgsT&&...args) {
    log::Logger::get().log_verbose(
        "[", workerId(), "] ",
        std::forward<ArgsT>(args)...
    );
  }

  static void dumpBuildPreamble(
      StringRef PreambleSource,
      StringRef PreambleOut
  ) {
    log_info(
        "PREAMBLE ", PreambleSource, " -> ",
        "preamble out: ", PreambleOut
    );
  }

  // TODO: Deprecarted
  static void dumpParse(
      StringRef OutASTFile,
      StringRef OutLDepsFile,
      StringRef SourceFile
  ) {
    with (auto info = log::Logger::get().acquire(log::Level::Info)) {
      auto &LogInfo = info.s;
      LogInfo
      << "PARSE     " << SourceFile << " -> "
      << "(ast:" << OutASTFile << ", "
      << "ldeps: " << OutLDepsFile << ")"
      << "\n";
    }
  }

  static void dumpParseImport(
      StringRef OutLDepsFile,
      StringRef SourceFile
  ) {
    log_info(
        "PARSE IMP ", SourceFile, " -> ",
        "(ldeps: ", OutLDepsFile, ")"
    );
  }

  // TODO Levitation: Deprecated
  static void dumpInstantiateDecl(
      StringRef OutDeclASTFile,
      StringRef InputObject,
      const Paths &Deps
  ) {
    assert(OutDeclASTFile.size() && InputObject.size());

    dumpInstantiate(OutDeclASTFile, InputObject, Deps, "INST DECL", "decl-ast");
  }

  static void dumpBuildDecl(
      StringRef OutDeclASTFile,
      StringRef OutDeclASTMetaFile,
      StringRef InputObject,
      const Paths &Deps
  ) {
    assert(OutDeclASTFile.size() && InputObject.size());

    // TODO Levitation: also dump OutDeclASTMetaFile

    dumpInstantiate(OutDeclASTFile, InputObject, Deps, "BUILD DECL", "decl-ast");
  }

  static void dumpBuildObject(
      StringRef OutObjFile,
      StringRef InputObject,
      const Paths &Deps
  ) {
    assert(OutObjFile.size() && InputObject.size());

    dumpInstantiate(OutObjFile, InputObject, Deps, "BUILD OBJ ", "object");
  }

  static void dumpInstantiateObject(
      StringRef OutObjFile,
      StringRef InputObject,
      const Paths &Deps
  ) {
    assert(OutObjFile.size() && InputObject.size());

    dumpInstantiate(OutObjFile, InputObject, Deps, "INST OBJ ", "object");
  }

  static void dumpCompileMain(
      StringRef OutObjFile,
      StringRef InputObject,
      const Paths &Deps
  ) {
    assert(OutObjFile.size() && InputObject.size());

    dumpInstantiate(OutObjFile, InputObject, Deps, "MAIN OBJ ", "object");
  }

  static void dumpInstantiate(
      StringRef OutDeclASTFile,
      StringRef InputObject,
      const Paths &Deps,
      StringRef ActionName,
      StringRef OutputName
  ) {
    assert(OutDeclASTFile.size() && InputObject.size());

    log_info(
        ActionName, " ", InputObject,
        ", ",
        dumpLDepsFiles(Deps),
        " -> ", OutputName, ": ", OutDeclASTFile
    );
  }

  static log::manipulator_t dumpLDepsFiles(
      const Paths &Deps
  ) {
    return dumpPathsArray(Deps, "deps");
  }

  static void dumpLink(StringRef OutputFile, const Paths &ObjectFiles) {
    assert(OutputFile.size() && ObjectFiles.size());
    log_info(
        "LINK ",
        dumpObjectFiles(ObjectFiles),
        " -> ", OutputFile
    );
  }

  static log::manipulator_t dumpObjectFiles(
      const Paths &ObjectFiles
  ) {
    return dumpPathsArray(ObjectFiles, "objects");
  }

  static log::manipulator_t dumpPathsArray(
      const Paths &ObjectFiles,
      StringRef ArrayName
  ) {
    return [=] (llvm::raw_ostream &out) {
      out << ArrayName << ": ";

      if (ObjectFiles.size()) {
        out << "(";
        for (size_t i = 0, e = ObjectFiles.size(); i != e; ++i) {
          out << ObjectFiles[i];
          if (i + 1 != e)
            out << ", ";
        }
        out << ")";
      } else {
        out << "<empty>";
      }
    };
  }

  static bool processStatus(const Failable &Status) {
    if (Status.hasWarnings())
      log::Logger::get().log_warning(Status.getWarningMessage());

    if (!Status.isValid()) {
      log::Logger::get().log_error(Status.getErrorMessage());
      return false;
    }

    return true;
  }
};

void LevitationDriverImpl::buildPreamble() {
  if (!Status.isValid())
    return;

  if (!Context.Driver.isPreambleCompilationRequested())
    return;

  if (Context.Driver.PreambleOutput.empty()) {
    Context.Driver.PreambleOutput = levitation::Path::getPath<SinglePath>(
      Context.Driver.BuildRoot,
      DriverDefaults::PREAMBLE_OUT
    );
    Context.Driver.PreambleOutputMeta = levitation::Path::getPath<SinglePath>(
      Context.Driver.BuildRoot,
      DriverDefaults::PREAMBLE_OUT_META
    );
  }

  DeclASTMeta Meta;
  if (isUpToDate(
      Meta,
      Context.Driver.PreambleOutput,
      Context.Driver.PreambleOutputMeta,
      Context.Driver.PreambleSource,
      Context.Driver.PreambleSource
  ))
    return;

  auto Res = Commands::buildPreamble(
    Context.Driver.BinDir,
    Context.Driver.Includes,
    Context.Driver.PreambleSource,
    Context.Driver.PreambleOutput,
    Context.Driver.PreambleOutputMeta,
    Context.Driver.StdLib,
    Context.Driver.ExtraPreambleArgs,
    Context.Driver.isVerbose(),
    Context.Driver.DryRun
  );

  if (!Res)
    Status.setFailure()
    << "Preamble: phase failed";

  setPreambleUpdated();
}

void LevitationDriverImpl::runParseImport() {
  auto &TM = TasksManager::get();

  for (auto PackagePath : Context.AllPackages) {

    auto &Files = Context.Files[PackagePath];

    DeclASTMeta ldepsMeta;

    if (isUpToDate(
        ldepsMeta,
        Files.LDeps,
        Files.LDepsMeta,
        Files.Source,
        Files.LDeps /* item description */
    ))
      continue;

    TM.runTask([=] (TasksManager::TaskContext &TC) {
      TC.Successful = Commands::parseImport(
          Context.Driver.BinDir,
          Context.Driver.PreambleOutput,
          Files.LDeps,
          Files.LDepsMeta,
          Files.Source,
          Context.Driver.SourcesRoot,
          Context.Driver.ExtraParseImportArgs,
          Context.Driver.isVerbose(),
          Context.Driver.DryRun
      );
    });
  }

  auto Res = TM.waitForTasks();

  if (!Res)
    Status.setFailure()
    << "Parse: phase failed.";
}

void LevitationDriverImpl::solveDependencies() {
  if (!Status.isValid())
    return;

  DependenciesSolver Solver;
  Solver.setSourcesRoot(Context.Driver.SourcesRoot);
  Solver.setBuildRoot(Context.Driver.BuildRoot);
  Solver.setVerbose(Context.Driver.isVerbose());

  Context.DependenciesInfo = Solver.solve(
      Context.ExternalPackages,
      Context.Files
  );

  Status.inheritResult(Solver, "Dependencies solver: ");
}

void LevitationDriverImpl::codeGen() {
  if (!Status.isValid())
    return;

  bool Res =
    Context.DependenciesInfo->getDependenciesGraph().dsfJobs(
        [&] (const DependenciesGraph::Node &N) {
          return processDependencyNode(N);
        }
    );

  if (!Res)
    Status.setFailure()
    << "Instantiate and codegen: phase failed.";
}

void LevitationDriverImpl::runLinker() {
  if (!Status.isValid())
    return;

  if (llvm::sys::fs::exists(Context.Driver.Output) && !Context.ObjectsUpdated) {
    Status.setWarning("Nothing to build.\n");
    return;
  }

  assert(Context.Driver.isLinkPhaseEnabled() && "Link phase must be enabled.");

  Paths ObjectFiles;
  for (auto &PackagePath : Context.ProjectPackages) {
    assert(Context.Files.count(PackagePath));
    ObjectFiles.push_back(Context.Files[PackagePath].Object);
  }

  auto Res = Commands::link(
      Context.Driver.BinDir,
      Context.Driver.Output,
      ObjectFiles,
      Context.Driver.StdLib,
      Context.Driver.ExtraLinkerArgs,
      Context.Driver.isVerbose(),
      Context.Driver.DryRun,
      Context.Driver.CanUseLibStdCppForLinker
  );

  if (!Res)
    Status.setFailure()
    << "Link: phase failed";
}

void LevitationDriverImpl::collectSources() {
  collectProjectSources();
  collectLibrariesSources();
}

void LevitationDriverImpl::collectProjectSources() {

  Log.log_verbose("Collecting project sources...");

  // Gather all .cppl files
  Paths ProjectPackages;

  FileSystem::collectFiles(
      ProjectPackages,
      Context.Driver.SourcesRoot,
      FileExtensions::SourceCode,
      /*ignore dirs*/ { Context.Driver.getBuildRoot() }
  );

  DenseMap<StringID, SinglePath> RelPaths;

  // Normalize all paths to project .cppl files and register them.
  for (const auto &Src : ProjectPackages) {
    auto Rel = levitation::Path::makeRelative<SinglePath>(
        Src, Context.Driver.SourcesRoot
    );

    auto UnitIdentifier = UnitIDUtils::fromRelPath(Rel);
    auto UnitID = Strings.addItem(StringRef(UnitIdentifier));
    RelPaths.try_emplace(UnitID, Rel);

    Context.ProjectPackages.insert(UnitID);
    Context.AllPackages.insert(UnitID);
  }

  for (auto UnitID : Context.ProjectPackages) {

    //const auto& UnitPath = *Strings.getItem(UnitID);
    const auto& PackagePath = RelPaths[UnitID];

    Log.log_trace("  Generating paths for '", PackagePath, "'...");


    auto &Files = Context.Files.create(UnitID);

    // In current implementation package path is equal to relative source path.

    Files.Source = Path::getPath<SinglePath>(
        Context.Driver.SourcesRoot,
        PackagePath,
        FileExtensions::SourceCode
    );

    Files.Header = Path::getPath<SinglePath>(
        Context.Driver.getOutputHeadersDir(),
        PackagePath,
        FileExtensions::Header
    );

    Files.Decl = Path::getPath<SinglePath>(
        Context.Driver.getOutputDeclsDir(),
        PackagePath,
        FileExtensions::SourceCode
    );

    SinglePath OutputTemplate = Path::getPath<SinglePath>(
        Context.Driver.BuildRoot,
        PackagePath
    );

    setOutputFilesInfo(Files, OutputTemplate, true);

    Files.dump(Log, log::Level::Trace, 4);
  }

  Log.log_verbose(
    "Found ", Context.ProjectPackages.size(),
    " '.", FileExtensions::SourceCode,
    "' project files.\n" // it was meant to put extra new line here
  );
}

void LevitationDriverImpl::collectLibrariesSources() {

  if (Context.Driver.LevitationLibs.empty())
    return;

  Log.log_verbose("Collecting libraries (presumable declaration) sources...");

  // Register all external packages.

  for (auto &CollectedExtLib : Context.Driver.LevitationLibs) {

    Path::Builder ExtLibPath;
    ExtLibPath
      .addComponent(Context.Driver.SourcesRoot)
      .addComponent(CollectedExtLib)
      .done();

    auto ExtLibAbsPath = Path::makeAbsolute<SinglePath>(ExtLibPath.str());

    Log.log_verbose("  Checking dir '", CollectedExtLib, "'...");
    Paths ExternalPackages;
    FileSystem::collectFiles(
        ExternalPackages,
        ExtLibAbsPath,
        FileExtensions::SourceCode,
        /*ignore dirs*/ { Context.Driver.getBuildRoot() }
    );

    for (const auto &CollectedPath : ExternalPackages) {
      auto PackagePath = Path::makeAbsolute<SinglePath>(CollectedPath);
      auto Package = Path::makeRelative<SinglePath>(PackagePath, ExtLibAbsPath);
      auto UnitIdentifier = UnitIDUtils::fromRelPath(Package);

      auto UnitID = Strings.addItem(StringRef(UnitIdentifier));

      Context.ExternalPackages.insert(UnitID);
      Context.AllPackages.insert(UnitID);

      Log.log_trace(
          "Checking lib package '", UnitIdentifier, "' -> '", PackagePath, "'..."
      );

      auto &Files = Context.Files.create(UnitID);

      // For libraries sources keep absolute source paths

      Files.Source = Path::replaceExtension<SinglePath>(
          PackagePath,
          FileExtensions::SourceCode
      );

      Path::Builder PBHeader;
      PBHeader
        .addComponent(Context.Driver.getOutputHeadersDir())
        .addComponent(Context.Driver.LibsOutSubDir)
        .addComponent(PackagePath)
        .replaceExtension(FileExtensions::Header)
        .done(Files.Header);

      // Note, we don't generate decl .cpp files for levitation libraries.
      // Source file itself is a decl file.

      SinglePath OutputTemplate;

      Path::Builder PBOutputTemplate;
      PBOutputTemplate
        .addComponent(Context.Driver.BuildRoot)
        .addComponent(Context.Driver.LibsOutSubDir)
        .addComponent(PackagePath)
        .done(OutputTemplate);

      setOutputFilesInfo(Files, OutputTemplate, false);

      Files.dump(Log, log::Level::Trace, 4);
    }
  }

  Log.log_verbose(
    "Found ", Context.ExternalPackages.size(),
    " '.", FileExtensions::SourceCode,
    "' files.\n" // it was meant to put extra new line here.
  );
}

void LevitationDriverImpl::setOutputFilesInfo(
    FilesInfo &Files,
    StringRef OutputPathWithoutExt,
    bool SetObjectRelatedInfo
) {
    // In current implementation package path is equal to relative source path.

    Files.DeclASTMetaFile = Path::replaceExtension<SinglePath>(
        OutputPathWithoutExt, FileExtensions::DeclASTMeta
    );

    Files.LDeps = Path::replaceExtension<SinglePath>(
        OutputPathWithoutExt, FileExtensions::ParsedDependencies
    );

    Files.LDepsMeta = Path::replaceExtension<SinglePath>(
        OutputPathWithoutExt, FileExtensions::ParsedDependenciesMeta
    );

    Files.DeclAST = Path::replaceExtension<SinglePath>(
        OutputPathWithoutExt, FileExtensions::DeclarationAST
    );

    if (SetObjectRelatedInfo) {
      Files.ObjMetaFile = Path::replaceExtension<SinglePath>(
          OutputPathWithoutExt, FileExtensions::ObjMeta
      );
      Files.Object = Path::replaceExtension<SinglePath>(
          OutputPathWithoutExt, FileExtensions::Object
      );
    }
}

// TODO Levitation: try to make this method const.
bool LevitationDriverImpl::processDependencyNode(
    const DependenciesGraph::Node &N
) {
  DeclASTMeta ExistingMeta;
  if (isUpToDate(ExistingMeta, N))
    return true;

  switch (N.Kind) {
    case DependenciesGraph::NodeKind::Declaration:
      return processDeclaration(ExistingMeta.getDeclASTHash(), N);
    case DependenciesGraph::NodeKind::Definition: {
      return processDefinition(N);
    }
    default:
      llvm_unreachable("Unknown dependency kind");
  }
}

const FilesInfo& LevitationDriverImpl::getFilesInfoFor(
    const DependenciesGraph::Node &N
) const {
  auto *FoundFiles = Context.Files.tryGet(N.LevitationUnit->UnitPath);
  if(!FoundFiles) {
    const auto &SrcRel = *Strings.getItem(N.LevitationUnit->UnitPath);
    Log.log_error(
        "Package '",
        SrcRel,
        "' is present in dependencies, but not found."
    );
    llvm_unreachable("Package not found");
  }
  return *FoundFiles;
}

Paths LevitationDriverImpl::getFullDependencies(
    const DependenciesGraph::Node &N,
    const DependenciesGraph &Graph
) const {
  auto &FullDepsRanged = Context.DependenciesInfo->getRangedDependencies(N.ID);

  Paths FullDeps;
  for (auto RangeNID : FullDepsRanged) {
    auto &DNode = Graph.getNode(RangeNID.second);
    auto DepPath = *Strings.getItem(DNode.LevitationUnit->UnitPath);

    auto &Files = Context.Files[DNode.LevitationUnit->UnitPath];

    FullDeps.push_back(Files.DeclAST);
  }
  return FullDeps;
}

Paths LevitationDriverImpl::getIncludeSources(
    const DependenciesGraph::Node &N,
    const DependenciesGraph &Graph
) const {
  Paths Includes;
  for (auto DepNID : N.Dependencies) {
    auto &DNode = Graph.getNode(DepNID);
    auto DepPath = Context.Files[DNode.LevitationUnit->UnitPath].Header;

    auto Header = Path::makeRelative<SinglePath>(
        DepPath,
        Context.Driver.getOutputHeadersDir()
    );

    Includes.emplace_back(std::move(Header));
  }
  return Includes;
}

Paths LevitationDriverImpl::getImportSources(
    const DependenciesGraph::Node &N,
    const DependenciesGraph &Graph
) const {
  Paths Imports;
  for (auto DepNID : N.Dependencies) {
    auto &DNode = Graph.getNode(DepNID);
    auto DepPackage = *Strings.getItem(DNode.LevitationUnit->UnitPath);

    // Remove extension, A/B/C.cppl -> A/B/C
    DepPackage = Path::replaceExtension<SinglePath>(DepPackage, "");

    Imports.push_back(DepPackage);
  }
  return Imports;
}

bool LevitationDriverImpl::processDefinition(
    const DependenciesGraph::Node &N
) {
  assert(
      N.Kind == DependenciesGraph::NodeKind::Definition &&
      "Only definition nodes expected here"
  );

  const auto &Graph = Context.DependenciesInfo->getDependenciesGraph();
  const auto &Files = getFilesInfoFor(N);

  Paths fullDependencies = getFullDependencies(N, Graph);

  setObjectsUpdated();

  StringRef UnitID = *Strings.getItem(N.LevitationUnit->UnitPath);

  return Commands::buildObject(
    Context.Driver.BinDir,
    Context.Driver.Includes,
    Context.Driver.PreambleOutput,
    Files.Object,
    Files.ObjMetaFile,
    Files.Source,
    UnitID,
    fullDependencies,
    Context.Driver.StdLib,
    Context.Driver.ExtraParseArgs,
    Context.Driver.ExtraCodeGenArgs,
    Context.Driver.isVerbose(),
    Context.Driver.DryRun
  );
}

bool LevitationDriverImpl::processDeclaration(
    HashRef OldDeclASTHash,
    const DependenciesGraph::Node &N
) {
  const auto &Graph = Context.DependenciesInfo->getDependenciesGraph();
  const auto &Files = getFilesInfoFor(N);
  Paths fullDependencies = getFullDependencies(N, Graph);

  bool NeedDeclAST = true;

  if (N.DependentNodes.empty() && !Graph.isPublic(N.ID)) {
    with (auto verb = Log.acquire(log::Level::Verbose)) {
      auto &Verbose = verb.s;
      Verbose << "TODO: Skip building unused declaration for ";
      Graph.dumpNodeShort(Verbose, N.ID, Strings);
      Verbose << "\n";
    }

    // TODO Levitation: see #48
    // NeedDeclAST = false;
  }

  StringRef UnitID = *Strings.getItem(N.LevitationUnit->UnitPath);

  // Check whether we also will compile a definition,
  // in this case both phases may produce same warnings,
  // so suppress warnings for declaration.
  //
  // NOTE: this is only actual unless we change parsing workflow.
  // In future I hope to parse definition with preincluded
  // parsed declaratino AST, in this case we should change this behaviour.
  bool SuppressLevitationWarnings = N.LevitationUnit->Definition != nullptr;
  auto ExtraArgs = Context.Driver.ExtraParseArgs;
  if (SuppressLevitationWarnings)
    ExtraArgs.emplace_back("-Wno-everything");

  bool buildDeclSuccessfull = Commands::buildDecl(
      Context.Driver.BinDir,
      Context.Driver.Includes,
      Context.Driver.PreambleOutput,
      (NeedDeclAST ? Files.DeclAST.str() : StringRef()),
      (NeedDeclAST ? Files.DeclASTMetaFile.str() : StringRef()),
      Files.Source,
      UnitID,
      fullDependencies,
      Context.Driver.StdLib,
      ExtraArgs,
      Context.Driver.isVerbose(),
      Context.Driver.DryRun
  );

  if (!buildDeclSuccessfull)
    return false;

  bool MustGenerateHeaders =
      Context.Driver.shouldCreateHeaders() &&
      Graph.isPublic(N.ID);

  bool MustGenerateDecl =
      Context.Driver.shouldCreateDecls() &&
      Graph.isPublic(N.ID) &&
      !Graph.isExternal(N.ID);

  DeclASTMeta Meta;
  if (!DeclASTMetaLoader::fromFile(
      Meta, Context.Driver.BuildRoot, Files.DeclASTMetaFile
  ))
    return false;

  bool Success = true;

  if (MustGenerateHeaders) {

    assert(!Files.Header.empty());

    auto IncludeSources = getIncludeSources(N, Graph);
    Success = HeaderGenerator(
        *Strings.getItem(N.LevitationUnit->UnitPath),
        Files.Header,
        Files.Source,
        N.Dependencies.empty() ? Context.Driver.PreambleSource : "",
        IncludeSources,
        Meta.getFragmentsToSkip(),
        Context.Driver.isVerbose(),
        Context.Driver.DryRun
    )
    .execute();
  }

  if (MustGenerateDecl) {

    assert(!Files.Decl.empty());

    auto DeclSources = getImportSources(N, Graph);

    Success = HeaderGenerator(
        *Strings.getItem(N.LevitationUnit->UnitPath),
        Files.Decl,
        Files.Source,
        N.Dependencies.empty() ? Context.Driver.PreambleSource : "",
        DeclSources,
        Meta.getFragmentsToSkip(),
        Context.Driver.isVerbose(),
        Context.Driver.DryRun,
        /*import*/true
    )
    .execute();
  }

  // Mark that node was updated, if it was updated

  if (!equal(OldDeclASTHash, Meta.getDeclASTHash()))
    setNodeUpdated(N.ID);
  else {
    with (auto verb = Log.acquire(log::Level::Verbose)) {
      auto &Verbose = verb.s;
      Verbose << "Node ";
      Graph.dumpNodeShort(Verbose, N.ID, Strings);
      Verbose << " is up-to-date.\n";
    }
  }

  return Success;
}

bool LevitationDriverImpl::isUpToDate(
    DeclASTMeta &Meta,
    const DependenciesGraph::Node &N
) {
  if (Context.PreambleUpdated)
    return false;

  for (auto D : N.Dependencies)
    if (Context.UpdatedNodes.count(D))
      return false;

  const auto &Files = getFilesInfoFor(N);

  StringRef MetaFile;
  StringRef ProductFile;

  switch (N.Kind) {
    case DependenciesGraph::NodeKind::Declaration:
      MetaFile = Files.DeclASTMetaFile;
      ProductFile = Files.DeclAST;
      break;
    case DependenciesGraph::NodeKind::Definition:
      MetaFile = Files.ObjMetaFile;
      ProductFile = Files.Object;
      break;
    default:
      return false;
  }

  auto NodeDescr = Context.DependenciesInfo->getDependenciesGraph()
      .nodeDescrShort(N.ID, Strings);

  return isUpToDate(Meta, ProductFile, MetaFile, Files.Source, NodeDescr);
}

bool LevitationDriverImpl::isUpToDate(
    DeclASTMeta &Meta,
    llvm::StringRef ProductFile,
    llvm::StringRef MetaFile,
    llvm::StringRef SourceFile,
    llvm::StringRef ItemDescr
) {
  if (!llvm::sys::fs::exists(MetaFile))
    return false;

  if (!llvm::sys::fs::exists(ProductFile))
    return false;

  if (!DeclASTMetaLoader::fromFile(
      Meta, Context.Driver.BuildRoot, MetaFile
  )) {
    Log.log_warning(
      "Failed to load existing meta file for '",
      SourceFile, "'\n",
      "  Must rebuild dependent chains."
    );
    return false;
  }

  // Get source MD5

  auto &FM = CreatableSingleton<FileManager>::get();

  if (auto Buffer = FM.getBufferForFile(SourceFile)) {
    auto SrcMD5 = calcMD5(Buffer->get()->getBuffer());

#if 0
    auto &Verbose = Log.verbose();
    Verbose << "Old Hash: ";
    for (auto b : Meta.getSourceHash()) {
      Verbose.write_hex(b);
      Verbose << " ";
    }

    Verbose << "\n";

    Verbose << "Src Hash: ";
    for (auto b : SrcMD5.Bytes) {
      Verbose.write_hex(b);
      Verbose << " ";
    }

    Verbose << "\n";
#endif

    // FIXME Levitation: we either should give up and remove this check
    //  or somehow separation md5 for source locations block
    //  and rest of decl-ast file.
    // Currently each time you change source, you change source locations.
    // So, even though declaration itself may remain same, .decl-ast
    // will be different.
    bool Res = equal(Meta.getSourceHash(), SrcMD5.Bytes);

    if (Res) {
      Log.log_verbose("Source  for item '", ItemDescr, "' is up-to-date.");
    }
    return Res;
  } else
    Log.log_warning(
      "Failed to load source '",
      SourceFile ,"' during up-to-date checks.\n",
      "  Must rebuild dependent chains. But I think I'll fail, dude..."
    );
  return false;
}

void LevitationDriverImpl::setPreambleUpdated() {
  Context.PreambleUpdated = true;
}

void LevitationDriverImpl::setNodeUpdated(DependenciesGraph::NodeID::Type NID) {
  Context.UpdatedNodes.insert(NID);
}


void LevitationDriverImpl::setObjectsUpdated() {
  Context.ObjectsUpdated = true;
}

//-----------------------------------------------------------------------------
//  LevitationDriver

LevitationDriver::LevitationDriver(StringRef CommandPath)
{
  SinglePath P = CommandPath;
  if (auto Err = llvm::sys::fs::make_absolute(P)) {
    log::Logger::get().log_warning(
      "Failed to make absolute path. System message: ",
      Err.message()
    );
    P = CommandPath;
  }

  BinDir = llvm::sys::path::parent_path(P);

#ifdef LEVITATION_DEFAULT_INCLUDES
  StringRef DefaultIncludes(LEVITATION_DEFAULT_INCLUDES);
  if (!DefaultIncludes.empty()) {

    SmallVector<StringRef, 16> DefaultIncludesItems;
    DefaultIncludes.split(DefaultIncludesItems, ';');

    for (auto DefaultInclude : DefaultIncludesItems)
      Includes.push_back(DefaultInclude.trim());
  }
#endif

  OutputHeadersDir = levitation::Path::getPath<SinglePath>(
      BuildRoot, DriverDefaults::HEADER_DIR_SUFFIX
  );

  OutputDeclsDir = levitation::Path::getPath<SinglePath>(
      BuildRoot, DriverDefaults::DECLS_DIR_SUFFIX
  );
}

void LevitationDriver::setExtraPreambleArgs(StringRef Args) {
  ExtraPreambleArgs = ArgsUtils::parse(Args);
}

void LevitationDriver::setExtraParserArgs(StringRef Args) {
  ExtraParseArgs = ArgsUtils::parse(Args);
}

void LevitationDriver::setExtraCodeGenArgs(StringRef Args) {
  ExtraCodeGenArgs = ArgsUtils::parse(Args);
}

void LevitationDriver::setExtraLinkerArgs(StringRef Args) {
  ExtraLinkerArgs = ArgsUtils::parse(Args);
}

bool LevitationDriver::run() {

  log::Logger::createLogger(log::Level::Info);
  TasksManager::create(JobsNumber-1);
  CreatableSingleton<FileManager>::create( FileSystemOptions { std::string(StringRef()) });
  CreatableSingleton<DependenciesStringsPool >::create();

  initParameters();

  RunContext Context(*this);
  LevitationDriverImpl Impl(Context);

  Impl.collectSources();
  Impl.buildPreamble();
  Impl.runParseImport();
  Impl.solveDependencies();
  Impl.codeGen();

  if (LinkPhaseEnabled)
    Impl.runLinker();

  if (Context.Status.hasWarnings()) {
    log::Logger::get().log_warning(Context.Status.getWarningMessage());
  }

  if (!Context.Status.isValid()) {
    log::Logger::get().log_error(Context.Status.getErrorMessage());
    return false;
  }

  return true;
}

void LevitationDriver::initParameters() {
  if (Output.empty()) {
    Output = isLinkPhaseEnabled() ?
        DriverDefaults::OUTPUT_EXECUTABLE :
        DriverDefaults::OUTPUT_OBJECTS_DIR;
  }

  switch (Verbose) {
    case VerboseLevel0:
      log::Logger::get().setLogLevel(log::Level::Info);
      break;
    case VerboseLevel1:
      log::Logger::get().setLogLevel(log::Level::Verbose);
      break;
    case VerboseLevel2:
      log::Logger::get().setLogLevel(log::Level::Trace);
      break;
  }

  if (isVerbose())
    dumpParameters();
}

void LevitationDriver::dumpParameters() {

  with (auto verb = log::Logger::get().acquire(log::Level::Verbose)) {
    auto &Out = verb.s;

    Out
    << "\n"
    << "  Running driver with following parameters:\n\n"
    << "    BinaryDir: " << BinDir << "\n"
    << "    SourcesRoot: " << SourcesRoot << "\n"
    << "    BuildRoot: " << BuildRoot << "\n"
    << "    PreambleSource: " << (PreambleSource.empty() ? "<preamble compilation not requested>" : PreambleSource)
    << "\n"
    << "    JobsNumber (including main thread): " << JobsNumber << "\n"
    << "    Output: " << Output << "\n"
    << "    OutputHeadersDir: " << (isLinkPhaseEnabled() ? "<n/a>" : OutputHeadersDir.c_str()) << "\n"
    << "    OutputDeclsDir: " << (isLinkPhaseEnabled() ? "<n/a>" : OutputDeclsDir.c_str()) << "\n"
    << "    DryRun: " << (DryRun ? "yes" : "no") << "\n"
    << "\n";

    dumpIncludes(Out);
    dumpExtraFlags(Out, "Preamble", ExtraPreambleArgs);
    dumpExtraFlags(Out, "Parse", ExtraParseArgs);
    dumpExtraFlags(Out, "CodeGen", ExtraCodeGenArgs);
    dumpExtraFlags(Out, "Link", ExtraLinkerArgs);

    Out << "\n";
  }
}

void LevitationDriver::dumpExtraFlags(
    llvm::raw_ostream& Out,
    StringRef Phase,
    const Args &args
) {

  if (args.empty())
    return;

  Out << "Extra args, phase '" << Phase << "':\n";
  Out << "  ";

  ArgsUtils::dump(Out, args);

  Out << "\n";
}

void LevitationDriver::dumpIncludes(llvm::raw_ostream &Out) {

  Out << "Includes: ";

  if (Includes.empty()) {
    Out << "<empty>\n";
    return;
  }


  Out << "\n";

  for (const auto &include : Includes) {
    Out.indent(2) << include << "\n";
  }

  Out << "\n";
}


}}}
