//==-- LevitationFrontendActions.cpp --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Frontend/LevitationFrontendActions.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTDiagnostic.h"
#include "clang/AST/ASTImporter.h"
#include "clang/AST/ASTImporterLookupTable.h"
#include "clang/Basic/FileManager.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/LevitationASTConsumers.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Frontend/Utils.h"
#include "clang/Levitation/FileExtensions.h"
#include "clang/Levitation/DeserializationListeners.h"
#include "clang/Levitation/Common/File.h"
#include "clang/Levitation/Common/WithOperator.h"
#include "clang/Levitation/Serialization.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Sema/TemplateInstCallback.h"
#include "clang/Serialization/ASTBitCodes.h"
#include "clang/Serialization/ASTReader.h"
#include "clang/Serialization/ASTWriter.h"
#include "clang/Serialization/GlobalModuleIndex.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/YAMLTraits.h"
#include <memory>
#include <system_error>
#include <utility>

using namespace clang;

namespace {

struct OutputFileHandle {
  std::unique_ptr<llvm::raw_pwrite_stream> OutputStream;
  std::string Path;

  static OutputFileHandle makeInvalid() {
    return {nullptr, ""};
  }

  bool isInvalid() const { return !(bool)OutputStream; };
};

template<class MultiplexT, class ItemT>
class MultiplexBuilder {

  bool Successful = true;

  typedef std::unique_ptr<ItemT> PtrTy;
  std::vector<PtrTy> Consumers;

public:

  using BuilderTy = MultiplexBuilder;

  BuilderTy &addRequired(PtrTy &&Consumer) {
    if (Successful && Consumer)
      Consumers.push_back(std::move(Consumer));
    else
      Successful = false;
    return *this;
  }

  BuilderTy &addNotNull(PtrTy &&Consumer) {
    assert(Consumer && "Consumer must be non-null pointer");
    return addRequired(std::move(Consumer));
  }

  BuilderTy &addOptional(PtrTy &&Consumer) {
    if (Consumer)
      addRequired(std::move(Consumer));

    return *this;
  }

  PtrTy done() {
    if (!Successful)
      return nullptr;

    if (Consumers.size() == 1)
      return std::move(Consumers[0]);

    return std::make_unique<MultiplexT>(std::move(Consumers));
  }
};


typedef MultiplexBuilder<MultiplexConsumer, ASTConsumer> MultiplexConsumerBuilder;

typedef MultiplexBuilder<
    levitation::LevitationMultiplexPreprocessorConsumer,
    levitation::LevitationPreprocessorConsumer
> MultiplexPPConsumerBuilder;

void diagMetaFileIOIssues(
    DiagnosticsEngine &Diag,
    levitation::File::StatusEnum status
) {
  switch (status) {
    case levitation::File::HasStreamErrors:
      Diag.Report(diag::err_fe_levitation_decl_ast_meta_file_io_troubles);
      break;
    case levitation::File::FiledToRename:
    case levitation::File::FailedToCreateTempFile:
      Diag.Report(diag::err_fe_levitation_decl_ast_meta_failed_to_create);
      break;
    default:
      break;
  }
}

template <typename EndSourceFileActionF>
void CreateMetaWrapper(
    FrontendAction &Action, EndSourceFileActionF &&EndSourceFileParentAction
) {
    // Remember everything we need for pos-processing.
  // After FrontendAction::EndSourceFile()
  // Compiler invocation and Sema will be destroyed.
  // Only SourceManager and FileManager remain.

  auto &CI = Action.getCompilerInstance();
  StringRef MetaOut = CI.getFrontendOpts().LevitationDeclASTMeta;
  auto &SM = CI.getSourceManager();
  auto &FM = SM.getFileManager();
  auto &Diag = CI.getDiagnostics();
  auto SkippedSrcFragments = CI.getSema().levitationGetSourceFragments();

  EndSourceFileParentAction();

  auto SrcBuffer = SM.getBufferData(SM.getMainFileID());
  std::unique_ptr<llvm::MemoryBuffer> OutBuffer;

  StringRef OutFile = CI.getCurrentOutputFilePath();

  assert(OutFile.size());

  if (auto OutBufferRes = FM.getBufferForFile(OutFile)) {
    OutBuffer = std::move(OutBufferRes.get());
  } else {
    Diag.Report(diag::err_fe_levitation_decl_ast_meta_failed_to_create)
    << OutFile;
    return;
  }

  auto SourceMD5 = levitation::calcMD5(SrcBuffer);
  auto OutputMD5 = levitation::calcMD5(OutBuffer->getBuffer());

  levitation::DeclASTMeta Meta(
    SourceMD5.Bytes,
    OutputMD5.Bytes,
    SkippedSrcFragments
  );

  assert(MetaOut.size());
  levitation::File F(MetaOut);

  if (auto OpenedFile = F.open()) {
    auto Writer = levitation::CreateMetaBitstreamWriter(OpenedFile.getOutputStream());
    Writer->writeAndFinalize(Meta);
  }

  if (F.hasErrors()) {
    diagMetaFileIOIssues(Diag, F.getStatus());
    return;
  }
}

} // end of anonymous namespace

namespace clang {

// Public classes implementation

void LevitationPreprocessorAction::EndSourceFileAction() {
  auto &PP = getCompilerInstance().getPreprocessor();

  auto Consumer = CreatePreprocessorConsumer();
  Consumer->HandlePreprocessor(PP);

  FrontendAction::EndSourceFileAction();
}

// Action implementations

// This class in friends of ASTReader, so it can't be
// allocated in anonymous namespace
class LevitationModulesReader : public ASTReader {
  StringRef MainFile;
  int MainFileChainIndex = -1;
DiagnosticsEngine &Diags;

  using OnFailFn = std::function<void(
      DiagnosticsEngine &,
      StringRef,
      ASTReader::ASTReadResult
  )>;
  OnFailFn OnFail;

public:

  LevitationModulesReader(
      CompilerInstance &CompilerInst,
      StringRef mainFile,
      OnFailFn &&onFail
  ) :
    ASTReader(
      CompilerInst.getPreprocessor(),
      CompilerInst.getModuleCache(),
      &CompilerInst.getASTContext(),
      CompilerInst.getPCHContainerReader(),
      {}
    ),
    MainFile(mainFile),
    Diags(CompilerInst.getDiagnostics()),
    OnFail(std::move(onFail))
  {
    LevitationMode = true;
    ModuleMgr.LevitationMode = true;
  }

  using OpenedScope = levitation::ScopeExit<std::function<void()>>;

  OpenedScope open() {
    auto Context = beginRead();
    return OpenedScope([this, Context] {
        close(Context);
    });
  }

  // close() method is private, see below

  bool hasErrors() const {
    return ReadResult != Success;
  }

  ASTReadResult getStatus() const {
    return ReadResult;
  }

  void readPreamble(StringRef Preamble) {
    if (hasErrors())
      return;

    ReadResult = read(Preamble, serialization::MK_Preamble);

    if (ReadResult != ASTReader::Success) {
      OnFail(Diags, Preamble, ReadResult);
    }
  }

  void readDependency(StringRef Dependency) {
    if (hasErrors())
      return;

    serialization::ModuleKind Kind = serialization::MK_LevitationDependency;

    ReadResult = Dependency == MainFile ?
        readMainFile() :
        read(Dependency, Kind);

    if (ReadResult != ASTReader::Success)
      OnFail(Diags, Dependency, ReadResult);
  }

private:

  unsigned NumModules = 0;
  unsigned PreviousGeneration;
  SmallVector<ImportedModule, 16> Loaded;
  ASTReadResult ReadResult;
  serialization::ModuleKind LastReadModuleKind = serialization::MK_MainFile;

  bool mainFileLoaded() {
    return MainFileChainIndex != -1;
  }

  ASTReadResult readMainFile() {
    MainFileChainIndex = ModuleMgr.size();
    return read(MainFile, serialization::MK_MainFile);
  }

  void close(const OpenedReaderContext &OpenedContext) {
    return endRead(OpenedContext);
  }

  OpenedReaderContext beginRead() {
    return BeginRead(
        PreviousGeneration,
        NumModules,
        SourceLocation(),
        ARR_None
    );
  }

  void endRead(const OpenedReaderContext &OpenedContext) {
    if (hasErrors())
      return;

    // Read main file after all dependencies.
    if (MainFile.size() && !mainFileLoaded())
      ReadResult = readMainFile();

    if (ReadResult == Success) {
      ReadResult = EndRead(
          OpenedContext,
          std::move(Loaded),
          LastReadModuleKind,
          SourceLocation(),
          ARR_None,
          PreviousGeneration,
          NumModules
      );
    }

    if (hasErrors())
      OnFail(Diags, MainFile, getStatus());
    else if (mainFileLoaded()){
      FileID MainFileID = ModuleMgr[MainFileChainIndex].OriginalSourceFileID;
      SourceMgr.setMainFileID(MainFileID);
    }
  }

  ASTReadResult read(
      StringRef FileName,
      serialization::ModuleKind Type
  ) {
    LastReadModuleKind = Type;

    switch (ASTReadResult ReadResult = ReadASTCore(
        FileName,
        Type,
        SourceLocation(),
        /*ImportedBy=*/nullptr,
        Loaded,
        /*ExpectedSize=*/0,
        /*ExpectedModTime=*/0,
        ASTFileSignature(),
        ARR_None
    )) {
    case Failure:
    case Missing:
    case OutOfDate:
    case VersionMismatch:
    case ConfigurationMismatch:
    case HadErrors: {
      return removeModulesAndReturn(ReadResult, NumModules);
    }
    case Success:
      return Success;
    }
  }
};

void LevitationBuildPreambleAction::EndSourceFileAction() {
  CreateMetaWrapper(*this, [&] { GeneratePCHAction::EndSourceFileAction(); });
}

std::unique_ptr<levitation::LevitationPreprocessorConsumer>
LevitationParseImportAction::CreatePreprocessorConsumer() {
  return MultiplexPPConsumerBuilder()
    .addRequired(levitation::CreateDependenciesASTProcessor(
         getCompilerInstance(),
         getCurrentFile()
     ))
  .done();
}

} // end of namespace clang

//==============================================================================
// Frontend Actions

void LevitationBuildObjectAction::ExecuteAction() {
  // ASTFrontendAction is used as source

  CompilerInstance &CI = getCompilerInstance();
  if (!CI.hasPreprocessor())
    llvm_unreachable("Only actions with preprocessor are supported.");

  // FIXME: Move the truncation aspect of this into Sema, we delayed this till
  // here so the source manager would be initialized.
  if (hasCodeCompletionSupport() &&
      !CI.getFrontendOpts().CodeCompletionAt.FileName.empty())
    CI.createCodeCompletionConsumer();

  // Use a code completion consumer?
  CodeCompleteConsumer *CompletionConsumer = nullptr;
  if (CI.hasCodeCompletionConsumer())
    CompletionConsumer = &CI.getCodeCompletionConsumer();

  if (!CI.hasSema())
    CI.createSema(getTranslationUnitKind(), CompletionConsumer);

  loadASTFiles();

  AdaptedAction->ExecuteAction();
  CI.getDiagnostics().getClient()->EndSourceFile();
}

const char *readerStatusToString(ASTReader::ASTReadResult Res) {
  // TODO Levitation: introduce string constants in .td file
  switch (Res) {
    case ASTReader::Success:
      return "Successfull??";

    case ASTReader::Failure:
      return "File seems to be currupted.";

    case ASTReader::Missing:
      return "File is missing.";

    case ASTReader::OutOfDate:
      return "File is out of date.";

    case ASTReader::VersionMismatch:
      return "The AST file was written by a different version of Clang";

    case ASTReader::ConfigurationMismatch:
      return "The AST file was written with a different language/target "
             " configuration.";

    case ASTReader::HadErrors:
      return "AST file has errors.";
  }
}

static void diagFailedToRead(
    DiagnosticsEngine &Diag,
    StringRef File,
    ASTReader::ASTReadResult Res
) {
  Diag.Report(diag::err_levitation_failed_to_read_pch)
  << File;
  (void)Res;
}

static void diagFailedToLoadASTFiles(
    DiagnosticsEngine &Diag,
    ASTReader::ASTReadResult Res
) {
  Diag.Report(diag::err_levitation_failed_to_load_ast_files);
  (void)Res;
}


void LevitationBuildObjectAction::loadASTFiles() {

  StringRef MainFile =
      getCurrentFileKind().getFormat() == InputKind::Precompiled ?
      getCurrentFile() :
      StringRef();

  if (MainFile.empty() && ASTFiles.empty() && PreambleFileName.empty())
    return;

  CompilerInstance &CI = getCompilerInstance();

  CI.getDiagnostics().getClient()->BeginSourceFile(
      CI.getASTContext().getLangOpts()
  );

  CI.getDiagnostics().SetArgToStringFn(
      &FormatASTNodeDiagnosticArgument,
      &CI.getASTContext()
  );

  IntrusiveRefCntPtr<DiagnosticIDs> DiagIDs(
      CI.getDiagnostics().getDiagnosticIDs()
  );

  ASTImporterLookupTable LookupTable(
      *CI.getASTContext().getTranslationUnitDecl()
  );

  IntrusiveRefCntPtr<LevitationModulesReader>
      Reader(new LevitationModulesReader(
          CI, MainFile, /*On Fail do*/ diagFailedToRead
      ));

  CI.getASTContext().setExternalSource(Reader);
  setupDeserializationListener(*Reader);

  with(auto Opened = Reader->open()) {

    if (PreambleFileName.size())
      Reader->readPreamble(PreambleFileName);

    for (const auto &Dep : ASTFiles) {
      Reader->readDependency(Dep);
    }
  }

  if (Reader->hasErrors())
    diagFailedToLoadASTFiles(CI.getDiagnostics(), Reader->getStatus());
}

void LevitationBuildObjectAction::setupDeserializationListener(
    clang::ASTReader &Reader
) {
  assert(
      Consumer &&
      "setupDeserializationListener is part of "
      "FrontendAction::Execute stage and requires "
      "ASTConsumer instance to be created"
  );

  CompilerInstance &CI = getCompilerInstance();

  ASTDeserializationListener *DeserialListener =
      Consumer->GetASTDeserializationListener();

  bool DeleteDeserialListener = false;

  if (CI.getPreprocessorOpts().DumpDeserializedPCHDecls) {
    DeserialListener = new levitation::DeserializedDeclsDumper(
        DeserialListener,
        DeleteDeserialListener
    );

    DeleteDeserialListener = true;
  }

  if (!CI.getPreprocessorOpts().DeserializedPCHDeclsToErrorOn.empty()) {

    DeserialListener = new levitation::DeserializedDeclsChecker(
        CI.getASTContext(),
        CI.getPreprocessorOpts().DeserializedPCHDeclsToErrorOn,
        DeserialListener,
        DeleteDeserialListener
    );

    DeleteDeserialListener = true;
  }

  Reader.setDeserializationListener(
      DeserialListener,
      DeleteDeserialListener
  );
}

std::unique_ptr<ASTConsumer>
LevitationBuildObjectAction::CreateASTConsumer(
    CompilerInstance &CI,
    StringRef InFile
) {
  std::unique_ptr<ASTConsumer> C = createASTConsumerInternal(CI, InFile);
  Consumer = C.get();
  return C;
}

std::unique_ptr<ASTConsumer>
LevitationBuildObjectAction::createASTConsumerInternal(
    CompilerInstance &CI,
    StringRef InFile
) {

  auto AdoptedConsumer = ASTMergeAction::CreateASTConsumer(CI, InFile);

  std::unique_ptr<ASTConsumer> AstPrinter;

  if (CI.getFrontendOpts().LevitationASTPrint) {
    std::error_code EC;
    std::unique_ptr<raw_ostream> OS(new llvm::raw_fd_ostream("-", EC));
      AstPrinter = CreateASTPrinter(std::move(OS), CI.getFrontendOpts().ASTDumpFilter);
  }

  return MultiplexConsumerBuilder()
    .addRequired(std::move(AdoptedConsumer))
    .addOptional(std::move(AstPrinter))
  .done();
}

void LevitationBuildObjectAction::EndSourceFileAction() {
  CreateMetaWrapper(*this, [&] { ASTMergeAction::EndSourceFileAction(); });
}
