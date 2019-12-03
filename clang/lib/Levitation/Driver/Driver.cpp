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

#include "clang/Levitation/Common/Failable.h"
#include "clang/Levitation/Common/File.h"
#include "clang/Levitation/Common/FileSystem.h"
#include "clang/Levitation/Common/SimpleLogger.h"
#include "clang/Levitation/Common/StringBuilder.h"
#include "clang/Levitation/DeclASTMeta/DeclASTMeta.h"
#include "clang/Levitation/DeclASTMeta/DeclASTMetaLoader.h"
#include "clang/Levitation/DependenciesSolver/DependenciesGraph.h"
#include "clang/Levitation/DependenciesSolver/DependenciesSolverPath.h"
#include "clang/Levitation/DependenciesSolver/DependenciesSolver.h"
#include "clang/Levitation/DependenciesSolver/SolvedDependenciesInfo.h"
#include "clang/Levitation/Driver/Driver.h"
#include "clang/Levitation/Driver/HeaderGenerator.h"
#include "clang/Levitation/FileExtensions.h"
#include "clang/Levitation/TasksManager/TasksManager.h"

#include "llvm/Support/Program.h"
#include "llvm/ADT/None.h"

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

  struct FilesInfo {
    SinglePath Source;
    SinglePath Header;
    SinglePath LDeps;
    SinglePath SkippedBytes;

    // FIXME Levitation: deprecated
    SinglePath AST;

    SinglePath DeclAST;
    SinglePath Object;
  };

  struct RunContext {

    // TODO Levitation: Introduce LevitationDriverOpts and use here
    // its reference instead.
    LevitationDriver &Driver;
    Failable Status;

    Paths Packages;

    // TODO Levitation: key may be an integer (string ID for PackagePath)
    llvm::DenseMap<StringRef, FilesInfo> Files;

    std::shared_ptr<SolvedDependenciesInfo> DependenciesInfo;

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
  void runParse();
  void runParseImport();
  void solveDependencies();
  void instantiateAndCodeGen();
  void codeGen();
  void runLinker();

  void collectSources();

  void addMainFileInfo();

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

  bool processDeclaration(const DependenciesGraph::Node &N);

  const FilesInfo& getFilesInfoFor(
      const DependenciesGraph::Node &N
  ) const;

  Paths getFullDependencies(
      const DependenciesGraph::Node &N,
      const DependenciesGraph &Graph
  ) const;

  Paths getIncludes(
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

    bool Condition = true;
    bool Verbose;
    bool DryRun;

    CommandInfo(
        SinglePath &&executablePath,
        bool verbose,
        bool dryRun
    )
    : ExecutablePath(std::move(executablePath)),
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
        StringRef StdLib,
        bool verbose,
        bool dryRun
    ) {
      auto Cmd = getClangXXCommand(
          BinDir, StdLib, verbose, dryRun
      );

      Cmd
      .addArg("-cppl-preamble");

      return Cmd;
    }

    // TODO Levitation: Deprecated
    static CommandInfo getParse(
        StringRef BinDir,
        StringRef PrecompiledPreamble,
        bool verbose,
        bool dryRun
    ) {
      auto Cmd = getClangXXCommand(
          BinDir, "-libstdc++", verbose, dryRun
      );

      Cmd.addArg("-cppl-parse");

      if (PrecompiledPreamble.size())
        Cmd.addKVArgEq("-cppl-include-preamble", PrecompiledPreamble);

      return Cmd;
    }
    static CommandInfo getParseImport(
        StringRef BinDir,
        StringRef PrecompiledPreamble,
        bool verbose,
        bool dryRun
    ) {

      auto Cmd = getClangXXCommand(
          BinDir, "", verbose, dryRun
      );

      Cmd.addArg("-cppl-import");

      if (PrecompiledPreamble.size())
        Cmd.addKVArgEq("-cppl-include-preamble", PrecompiledPreamble);

      return Cmd;
    }
    static CommandInfo getInstDecl(
        StringRef BinDir,
        StringRef PrecompiledPreamble,
        bool verbose,
        bool dryRun
    ) {
      auto Cmd = getClangXXCommand(BinDir, "", verbose, dryRun);
      Cmd
      .addArg("-cppl-inst-decl");
      return Cmd;
    }

    static CommandInfo getInstObj(
        StringRef BinDir,
        StringRef PrecompiledPreamble,
        bool verbose,
        bool dryRun
    ) {
      auto Cmd = getClangXXCommand(BinDir, "", verbose, dryRun);
      Cmd
      .addArg("-cppl-compile");
      return Cmd;
    }

    static CommandInfo getBuildDecl(
        StringRef BinDir,
        StringRef PrecompiledPreamble,
        StringRef StdLib,
        bool verbose,
        bool dryRun
    ) {
      auto Cmd = getClangXXCommand(BinDir, StdLib, verbose, dryRun);
      Cmd
      .addArg("-cppl-decl");
      return Cmd;
    }

    static CommandInfo getBuildObj(
        StringRef BinDir,
        StringRef PrecompiledPreamble,
        StringRef StdLib,
        bool verbose,
        bool dryRun
    ) {
      auto Cmd = getClangXXCommand(BinDir, StdLib, verbose, dryRun);
      Cmd
      .addArg("-cppl-obj");
      return Cmd;
    }

    static CommandInfo getCompileSrc(
        StringRef BinDir,
        StringRef PrecompiledPreamble,
        bool verbose,
        bool dryRun
    ) {
      auto Cmd = getClangXXCommand(BinDir, "", verbose, dryRun);

      Cmd.addArg("-cppl-compile");

      if (PrecompiledPreamble.size())
        Cmd.addKVArgEq("-cppl-include-preamble", PrecompiledPreamble);

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

      if (!CanUseLibStdCpp)
        Cmd.addArg("-stdlib=libc++");
      else
        Cmd.addKVArgEqIfNotEmpty("-stdlib", StdLib);

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
    CommandInfo& addKVArgsEq(StringRef Name, const ValuesT Values) {
      if (!Condition) return *this;
      for (const auto &Value : Values) {
        OwnArgs.emplace_back((Name + "=" + Value).str());
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

    void dumpCommand() {

      auto &Out = log::Logger::get().info();

      for (unsigned i = 0, e = CommandArgs.size(); i != e; ++i) {
        if (i != 0)
          Out << " ";
        Out << CommandArgs[i];
      }

      Out << "\n";
    }
  };

  static bool parse(
      StringRef BinDir,
      StringRef PrecompiledPreamble,
      StringRef OutASTFile,
      StringRef OutLDepsFile,
      StringRef SourceFile,
      StringRef SourcesRoot,
      const LevitationDriver::Args &ExtraArgs,
      bool Verbose,
      bool DryRun
  ) {
    if (!DryRun || Verbose)
      dumpParse(OutASTFile, OutLDepsFile, SourceFile);

    levitation::Path::createDirsForFile(OutASTFile);
    levitation::Path::createDirsForFile(OutLDepsFile);

    auto ExecutionStatus = CommandInfo::getParse(
        BinDir, PrecompiledPreamble, Verbose, DryRun
    )
    .addKVArgEq("-cppl-src-root", SourcesRoot)
    .addKVArgEq("-cppl-deps-out", OutLDepsFile)
    .addArgs(ExtraArgs)
    .addArg(SourceFile)
    .addKVArgSpace("-o", OutASTFile)
    .execute();

    return processStatus(ExecutionStatus);
  }

  static bool parseImport(
      StringRef BinDir,
      StringRef PrecompiledPreamble,
      StringRef OutLDepsFile,
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
    .addArgs(ExtraArgs)
    .addArg(SourceFile)
    .execute();

    return processStatus(ExecutionStatus);
  }

  static bool instantiateDecl(
      StringRef BinDir,
      StringRef PrecompiledPreamble,
      StringRef OutDeclASTFile,
      StringRef InputObject,
      const Paths &Deps,
      const LevitationDriver::Args &ExtraArgs,
      bool Verbose,
      bool DryRun
  ) {
    assert(OutDeclASTFile.size() && InputObject.size());

    if (!DryRun || Verbose)
      dumpInstantiateDecl(OutDeclASTFile, InputObject, Deps);

    levitation::Path::createDirsForFile(OutDeclASTFile);

    auto ExecutionStatus = CommandInfo::getInstDecl(
        BinDir, PrecompiledPreamble, Verbose, DryRun
    )
    .addKVArgEqIfNotEmpty("-cppl-include-preamble", PrecompiledPreamble)
    .addKVArgsEq("-cppl-include-dependency", Deps)
    .addArgs(ExtraArgs)
    .addArg(InputObject)
    .addKVArgSpace("-o", OutDeclASTFile)
    .execute();

    return processStatus(ExecutionStatus);
  }

  static bool instantiateObject(
      StringRef BinDir,
      StringRef PrecompiledPreamble,
      StringRef OutObjFile,
      StringRef InputObject,
      const Paths &Deps,
      const LevitationDriver::Args &ExtraArgs,
      bool Verbose,
      bool DryRun
  ) {
    assert(OutObjFile.size() && InputObject.size());

    if (!DryRun || Verbose)
      dumpInstantiateObject(OutObjFile, InputObject, Deps);

    levitation::Path::createDirsForFile(OutObjFile);

    auto ExecutionStatus = CommandInfo::getInstObj(
        BinDir, PrecompiledPreamble, Verbose, DryRun
    )
    .addKVArgEqIfNotEmpty("-cppl-include-preamble", PrecompiledPreamble)
    .addKVArgsEq("-cppl-include-dependency", Deps)
    .addArgs(ExtraArgs)
    .addArg(InputObject)
    .addKVArgSpace("-o", OutObjFile)
    .execute();

    return processStatus(ExecutionStatus);
  }

  static bool buildDecl(
      StringRef BinDir,
      StringRef PrecompiledPreamble,
      StringRef OutDeclASTFile,
      StringRef InputFile,
      const Paths &Deps,
      StringRef StdLib,
      const LevitationDriver::Args &ExtraParserArgs,
      bool Verbose,
      bool DryRun
  ) {
    assert(OutDeclASTFile.size() && InputFile.size());

    if (!DryRun || Verbose)
      dumpBuildDecl(OutDeclASTFile, InputFile, Deps);

    levitation::Path::createDirsForFile(OutDeclASTFile);

    auto ExecutionStatus = CommandInfo::getBuildDecl(
        BinDir, PrecompiledPreamble, StdLib, Verbose, DryRun
    )
    .addKVArgEqIfNotEmpty("-cppl-include-preamble", PrecompiledPreamble)
    .addKVArgsEq("-cppl-include-dependency", Deps)
    .addArgs(ExtraParserArgs)
    .addArg(InputFile)
    // TODO Levitation: don't emit .decl-ast files
    //  in some cases. See task #48
    //    .condition(OutDeclASTFile.size())
    //        .addKVArgSpace("-o", OutDeclASTFile)
    //    .conditionElse()
    //        .addArg("-cppl-no-out")
    //    .conditionEnd()
    .addKVArgSpace("-o", OutDeclASTFile)
    .execute();

    return processStatus(ExecutionStatus);
  }

  static bool buildObject(
      StringRef BinDir,
      StringRef PrecompiledPreamble,
      StringRef OutObjFile,
      StringRef InputObject,
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
        BinDir, PrecompiledPreamble, StdLib, Verbose, DryRun
    )
    .addKVArgEqIfNotEmpty("-cppl-include-preamble", PrecompiledPreamble)
    .addKVArgsEq("-cppl-include-dependency", Deps)
    .addArgs(ExtraParserArgs)
    .addArgs(ExtraCodeGenArgs)
    .addArg(InputObject)
    .addKVArgSpace("-o", OutObjFile)
    .execute();

    return processStatus(ExecutionStatus);
  }

  static bool buildPreamble(
      StringRef BinDir,
      StringRef PreambleSource,
      StringRef PCHOutput,
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
        BinDir, StdLib, Verbose, DryRun
    )
    .addArg(PreambleSource)
    .addKVArgSpace("-o", PCHOutput)
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

  // FIXME Levitation: Deprecated
  //   all dump methods below are deprecated,
  //   use DriverPhaseDump methods instead.

  static void dumpBuildPreamble(
      StringRef PreambleSource,
      StringRef PreambleOut
  ) {
    auto &LogInfo = log::Logger::get().info();
    LogInfo
    << "PREAMBLE " << PreambleSource << " -> "
    << "preamble out: " << PreambleOut << "\n";
  }

  static void dumpParse(
      StringRef OutASTFile,
      StringRef OutLDepsFile,
      StringRef SourceFile
  ) {
    auto &LogInfo = log::Logger::get().info();
    LogInfo
    << "PARSE     " << SourceFile << " -> "
    << "(ast:" << OutASTFile << ", "
    << "ldeps: " << OutLDepsFile << ")"
    << "\n";
  }

  static void dumpParseImport(
      StringRef OutLDepsFile,
      StringRef SourceFile
  ) {
    auto &LogInfo = log::Logger::get().info();
    LogInfo
    << "PARSE IMP " << SourceFile << " -> "
    << "(ldeps: " << OutLDepsFile << ")"
    << "\n";
  }


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
      StringRef InputObject,
      const Paths &Deps
  ) {
    assert(OutDeclASTFile.size() && InputObject.size());

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

    auto &LogInfo = log::Logger::get().info();
    LogInfo << ActionName << " " << InputObject;

    LogInfo << ", ";
    dumpLDepsFiles(LogInfo, Deps);

    LogInfo << " -> " << OutputName << ": " << OutDeclASTFile << "\n";
  }

  static void dumpLDepsFiles(
      raw_ostream &Out,
      const Paths &Deps
  ) {
    dumpPathsArray(Out, Deps, "deps");
  }

  static void dumpLink(StringRef OutputFile, const Paths &ObjectFiles) {
    assert(OutputFile.size() && ObjectFiles.size());

    auto &LogInfo = log::Logger::get().info();

    LogInfo << "LINK ";

    dumpObjectFiles(LogInfo, ObjectFiles);

    LogInfo << " -> " << OutputFile << "\n";
  }

  static void dumpObjectFiles(
      raw_ostream &Out,
      const Paths &ObjectFiles
  ) {
    dumpPathsArray(Out, ObjectFiles, "objects");
  }

  static void dumpPathsArray(
      raw_ostream &Out,
      const Paths &ObjectFiles,
      StringRef ArrayName
  ) {
    Out << ArrayName << ": ";

    if (ObjectFiles.size()) {
      Out << "(";
      for (size_t i = 0, e = ObjectFiles.size(); i != e; ++i) {
        log::Logger::get().info() << ObjectFiles[i];
        if (i + 1 != e)
          log::Logger::get().info() << ", ";
      }
      Out << ")";
    } else {
      Out << "<empty>";
    }
  }

  static bool processStatus(const Failable &Status) {
    if (Status.hasWarnings())
      log::Logger::get().warning() << Status.getWarningMessage();

    if (!Status.isValid()) {
      log::Logger::get().error() << Status.getErrorMessage();
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
  }

  auto Res = Commands::buildPreamble(
    Context.Driver.BinDir,
    Context.Driver.PreambleSource,
    Context.Driver.PreambleOutput,
    Context.Driver.StdLib,
    Context.Driver.ExtraPreambleArgs,
    Context.Driver.Verbose,
    Context.Driver.DryRun
  );

  if (!Res)
    Status.setFailure()
    << "Preamble: phase failed";
}

// TODO Levitation: deprecated
void LevitationDriverImpl::runParse() {
  auto &TM = TasksManager::get();

  for (auto PackagePath : Context.Packages) {

    auto Files = Context.Files[PackagePath];

    TM.addTask([=] (TasksManager::TaskContext &TC) {
      TC.Successful = Commands::parse(
          Context.Driver.BinDir,
          Context.Driver.PreambleOutput,
          Files.AST,
          Files.LDeps,
          Files.Source,
          Context.Driver.SourcesRoot,
          Context.Driver.ExtraParseArgs,
          Context.Driver.Verbose,
          Context.Driver.DryRun
      );
    });
  }

  auto Res = TM.waitForTasks();

  if (!Res)
    Status.setFailure()
    << "Parse: phase failed.";
}

void LevitationDriverImpl::runParseImport() {
  auto &TM = TasksManager::get();

  for (auto PackagePath : Context.Packages) {

    auto Files = Context.Files[PackagePath];

    TM.addTask([=] (TasksManager::TaskContext &TC) {
      TC.Successful = Commands::parseImport(
          Context.Driver.BinDir,
          Context.Driver.PreambleOutput,
          Files.LDeps,
          Files.Source,
          Context.Driver.SourcesRoot,
          Context.Driver.ExtraParseImportArgs,
          Context.Driver.Verbose,
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
  Solver.setVerbose(Context.Driver.Verbose);

  Paths LDepsFiles;
  for (auto &PackagePath : Context.Packages) {
    assert(Context.Files.count(PackagePath));
    LDepsFiles.push_back(Context.Files[PackagePath].LDeps);
  }

  Context.DependenciesInfo = Solver.solve(LDepsFiles);

  Status.inheritResult(Solver, "Dependencies solver: ");
}

// TODO Levitation: deprecated
void LevitationDriverImpl::instantiateAndCodeGen() {
  if (!Status.isValid())
    return;

  bool Res =
    Context.DependenciesInfo->getDependenciesGraph().dsfJobs(
        [&] (const DependenciesGraph::Node &N) {
          return processDependencyNodeDeprecated(N);
        }
    );

  if (!Res)
    Status.setFailure()
    << "Instantiate and codegen: phase failed.";
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

  assert(Context.Driver.isLinkPhaseEnabled() && "Link phase must be enabled.");

  Paths ObjectFiles;
  for (auto &PackagePath : Context.Packages) {
    assert(Context.Files.count(PackagePath));
    ObjectFiles.push_back(Context.Files[PackagePath].Object);
  }

  auto Res = Commands::link(
      Context.Driver.BinDir,
      Context.Driver.Output,
      ObjectFiles,
      Context.Driver.StdLib,
      Context.Driver.ExtraLinkerArgs,
      Context.Driver.Verbose,
      Context.Driver.DryRun,
      Context.Driver.CanUseLibStdCppForLinker
  );

  if (!Res)
    Status.setFailure()
    << "Link: phase failed";
}

void LevitationDriverImpl::collectSources() {

  Log.verbose() << "Collecting sources...\n";

  // Gather all .cppl files
  FileSystem::collectFiles(
      Context.Packages,
      Context.Driver.SourcesRoot,
      FileExtensions::SourceCode
  );

  // Normalize all paths to .cppl files
  for (auto &Src : Context.Packages) {
    Src = levitation::Path::makeRelative<SinglePath>(
        Src, Context.Driver.SourcesRoot
    );
  }

  for (const auto &PackagePath : Context.Packages) {

    FilesInfo Files;

    // In current implementation package path is equal to relative source path.

    Files.Source = Path::getPath<SinglePath>(
        Context.Driver.SourcesRoot,
        PackagePath,
        FileExtensions::SourceCode
    );

    Files.Header = Path::getPath<SinglePath>(
        Context.Driver.SourcesRoot,
        PackagePath,
        FileExtensions::Header
    );

    Files.SkippedBytes = Path::getPath<SinglePath>(
        Context.Driver.SourcesRoot,
        PackagePath,
        FileExtensions::SkippedBytes
    );

    Files.LDeps = Path::getPath<SinglePath>(
        Context.Driver.BuildRoot,
        PackagePath,
        FileExtensions::ParsedDependencies
    );

    Files.DeclAST = Path::getPath<SinglePath>(
        Context.Driver.BuildRoot,
        PackagePath,
        FileExtensions::DeclarationAST
    );
    Files.Object = Path::getPath<SinglePath>(
        Context.Driver.BuildRoot,
        PackagePath,
        FileExtensions::Object
    );

    auto Res = Context.Files.insert({ PackagePath, Files });

    assert(Res.second);
  }

  Log.verbose()
  << "Found " << Context.Packages.size()
  << " '." << FileExtensions::SourceCode << "' files.\n\n";
}

// TODO Levitation: try to make this method const.
bool LevitationDriverImpl::processDependencyNode(
    const DependenciesGraph::Node &N
) {
  switch (N.Kind) {
    case DependenciesGraph::NodeKind::Declaration:
      return processDeclaration(N);
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
  const auto &SrcRel = *Strings.getItem(N.PackageInfo->PackagePath);

  auto FoundFiles = Context.Files.find(SrcRel);
  if(FoundFiles == Context.Files.end()) {
    Log.error()
    << "Package '" << SrcRel << "' is present in dependencies, but not found.\n";
    llvm_unreachable("Package not found");
  }

  return FoundFiles->second;
}

Paths LevitationDriverImpl::getFullDependencies(
    const DependenciesGraph::Node &N,
    const DependenciesGraph &Graph
) const {
  auto &FullDepsRanged = Context.DependenciesInfo->getRangedDependencies(N.ID);

  Paths FullDeps;
  for (auto RangeNID : FullDepsRanged) {
    auto &DNode = Graph.getNode(RangeNID.second);
    auto DepPath = *Strings.getItem(DNode.PackageInfo->PackagePath);

    DependenciesSolverPath::addDepPathsFor(
        FullDeps,
        Context.Driver.BuildRoot,
        DepPath
    );
  }
  return FullDeps;
}

Paths LevitationDriverImpl::getIncludes(
    const DependenciesGraph::Node &N,
    const DependenciesGraph &Graph
) const {
  Paths Includes;
  for (auto DepNID : N.Dependencies) {
    auto &DNode = Graph.getNode(DepNID);
    auto DepPath = *Strings.getItem(DNode.PackageInfo->PackagePath);

    DependenciesSolverPath::addIncPathsFor(
        Includes,
        Context.Driver.BuildRoot,
        DepPath
    );
  }
  return Includes;
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

  return Commands::buildObject(
    Context.Driver.BinDir,
    Context.Driver.PreambleOutput,
    Files.Object,
    Files.Source,
    fullDependencies,
    Context.Driver.StdLib,
    Context.Driver.ExtraParseArgs,
    Context.Driver.ExtraCodeGenArgs,
    Context.Driver.Verbose,
    Context.Driver.DryRun
  );
}

bool LevitationDriverImpl::processDeclaration(
    const DependenciesGraph::Node &N
) {

  const auto &Graph = Context.DependenciesInfo->getDependenciesGraph();
  const auto &Files = getFilesInfoFor(N);
  Paths fullDependencies = getFullDependencies(N, Graph);

  bool NeedDeclAST = true;

  if (N.DependentNodes.empty()) {
    auto &Verbose = Log.verbose();
    Verbose << "Skip building unused declaration for ";
    Graph.dumpNodeShort(Verbose, N.ID, Strings);
    Verbose << "\n";
    NeedDeclAST = false;
  }

  // TODO Levitation: MD5 check
  // 1. DeclASTMeta MetaOld = DeclASTMetaLoader::fromFile(Files.SkippedBytes);
  // 2. Calc source file MD5
  // 3. Compare source MD5 hashes, if they differ, build new decl-ast.

  bool buildDeclSuccessfull = Commands::buildDecl(
      Context.Driver.BinDir,
      Context.Driver.PreambleOutput,
      (NeedDeclAST ? Files.DeclAST.str() : StringRef()),
      Files.Source,
      fullDependencies,
      Context.Driver.StdLib,
      Context.Driver.ExtraParseArgs,
      Context.Driver.Verbose,
      Context.Driver.DryRun
  );

  if (!buildDeclSuccessfull)
    return false;

  bool MustGenerateHeaders =
      Context.Driver.shouldCreateHeaders() &&
      Graph.isPublic(N.ID);

  if (!MustGenerateHeaders)
    return true;

  auto Includes = getIncludes(N, Graph);

  DeclASTMeta Meta;
  if (!DeclASTMetaLoader::fromFile(
      Meta, Context.Driver.BuildRoot, Files.SkippedBytes
  ))
    return false;

  return HeaderGenerator(
      Context.Driver.SourcesRoot,
      Context.Driver.OutputHeadersDir,
      Files.Header,
      Files.Source,
      Includes,
      Meta.getSkippedBytes(),
      Context.Driver.Verbose,
      Context.Driver.DryRun
  )
  .execute();
}

// TODO Levitation: Deprecated
bool LevitationDriverImpl::processDependencyNodeDeprecated(
    const DependenciesGraph::Node &N
) {
  const auto &Strings = CreatableSingleton<DependenciesStringsPool>::get();
  const auto &Graph = Context.DependenciesInfo->getDependenciesGraph();

  const auto &SrcRel = *Strings.getItem(N.PackageInfo->PackagePath);

  auto FoundFiles = Context.Files.find(SrcRel);
  if(FoundFiles == Context.Files.end()) {
    Log.error()
    << "Package '" << SrcRel << "' is present in dependencies, but not found.\n";
    llvm_unreachable("Package not found");
  }

  const auto &Files = FoundFiles->second;

  auto &RangedDeps = Context.DependenciesInfo->getRangedDependencies(N.ID);

  Paths fullDependencies;
  for (auto RangeNID : RangedDeps) {
    auto &DNode = Graph.getNode(RangeNID.second);
    auto DepPath = *Strings.getItem(DNode.PackageInfo->PackagePath);

    DependenciesSolverPath::addDepPathsForDeprecated(
        fullDependencies,
        Context.Driver.BuildRoot,
        DepPath
    );
  }

  switch (N.Kind) {

    case DependenciesGraph::NodeKind::Declaration:
      return Commands::instantiateDecl(
          Context.Driver.BinDir,
          Context.Driver.PreambleOutput,
          Files.DeclAST,
          Files.AST,
          fullDependencies,
          Context.Driver.ExtraCodeGenArgs,
          Context.Driver.Verbose,
          Context.Driver.DryRun
      );

    case DependenciesGraph::NodeKind::Definition: {
      return Commands::instantiateObject(
        Context.Driver.BinDir,
        Context.Driver.PreambleOutput,
        Files.Object,
        Files.AST,
        fullDependencies,
        Context.Driver.ExtraCodeGenArgs,
        Context.Driver.Verbose,
        Context.Driver.DryRun
      );
    }

    default:
      llvm_unreachable("Unknown dependency kind");
  }
}

//-----------------------------------------------------------------------------
//  LevitationDriver

LevitationDriver::LevitationDriver(StringRef CommandPath)
{
  SinglePath P = CommandPath;
  if (auto Err = llvm::sys::fs::make_absolute(P)) {
    log::Logger::get().warning()
    << "Failed to make absolute path. System message: "
    << Err.message() << "\n";
    P = CommandPath;
  }

  BinDir = llvm::sys::path::parent_path(P);

  OutputHeadersDir = levitation::Path::getPath<SinglePath>(
      BinDir, DriverDefaults::HEADER_DIR_SUFFIX
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
  TasksManager::create(JobsNumber);
  CreatableSingleton<FileManager>::create( FileSystemOptions { StringRef() });
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
    log::Logger::get().warning()
    << Context.Status.getWarningMessage();
  }

  if (!Context.Status.isValid()) {
    log::Logger::get().error()
    << Context.Status.getErrorMessage();
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

  if (Verbose) {
    log::Logger::get().setLogLevel(log::Level::Verbose);
    dumpParameters();
  }
}

void LevitationDriver::dumpParameters() {

  auto &Out = log::Logger::get().verbose();

  Out
  << "\n"
  << "  Running driver with following parameters:\n\n"
  << "    BinaryDir: " << BinDir << "\n"
  << "    SourcesRoot: " << SourcesRoot << "\n"
  << "    PreambleSource: " << (PreambleSource.empty() ? "<preamble compilation not requested>" : PreambleSource) << "\n"
  << "    JobsNumber: " << JobsNumber << "\n"
  << "    Output: " << Output << "\n"
  << "    OutputHeadersDir: " << (OutputHeadersDir.empty() ? "<header creation not requested>" : OutputHeadersDir) << "\n"
  << "    DryRun: " << (DryRun ? "yes" : "no") << "\n"
  << "\n";

  dumpExtraFlags("Preamble", ExtraPreambleArgs);
  dumpExtraFlags("Parse", ExtraParseArgs);
  dumpExtraFlags("CodeGen", ExtraCodeGenArgs);
  dumpExtraFlags("Link", ExtraLinkerArgs);

  Out << "\n";
}

void LevitationDriver::dumpExtraFlags(StringRef Phase, const Args &args) {

  if (args.empty())
    return;

  auto &Out = log::Logger::get().verbose();

  Out << "Extra args, phase '" << Phase << "':\n";

  Out << "  ";

  ArgsUtils::dump(Out, args);

  Out << "\n";
}

}}}
