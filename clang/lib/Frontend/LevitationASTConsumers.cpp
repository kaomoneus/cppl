//==-- LevitationASTConsumers.cpp -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
//
// Levitation AST Consumers Implementations.
//
//===----------------------------------------------------------------------===//

#include "clang/Frontend/LevitationASTConsumers.h"
#include "clang/AST/AST.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticFrontend.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Levitation/Common/File.h"
#include "clang/Levitation/Common/Path.h"
#include "clang/Levitation/Common/Utility.h"
#include "clang/Levitation/DeclASTMeta/DeclASTMeta.h"
#include "clang/Levitation/Dependencies.h"
#include "clang/Levitation/FileExtensions.h"
#include "clang/Levitation/Serialization.h"
#include "clang/Lex/Preprocessor.h"

#include "llvm/Bitstream/BitstreamWriter.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"

#include <utility>

namespace clang {
namespace levitation {

  void LevitationMultiplexPreprocessorConsumer::HandlePreprocessor(
      clang::Preprocessor &PP
  ) {
    for (auto &ConsumerPP : Consumers)
      ConsumerPP->HandlePreprocessor(PP);
  }

  class ASTDependenciesProcessor : public LevitationPreprocessorConsumer {
    const CompilerInstance &CI;
  public:
    ASTDependenciesProcessor(const CompilerInstance &ci) : CI(ci) {}

    void HandlePreprocessor(Preprocessor &PP) override {

      PackageDependencies& Dependencies = PP.accessLevitationDependencies();

      auto F = createFile();

      if (auto OpenedFile = F.open()) {
        buffer_ostream buffer(OpenedFile.getOutputStream());
        auto Writer = CreateBitstreamWriter(buffer);
        Writer->writeAndFinalize(Dependencies);

        writeMeta(
            CI.getFrontendOpts().LevitationDeclASTMeta,
            buffer.str()
        );
      }

      if (F.hasErrors()) {
        diagDependencyFileIOIssues(F.getStatus());
      }
    }
  private:
    File createFile() {
      return File(CI.getFrontendOpts().LevitationDependenciesOutputFile);
    }

    void writeMeta(StringRef MetaOut, StringRef LDepsBuffer) {
      // Remember everything we need for pos-processing.
      // After FrontendAction::EndSourceFile()
      // Compiler invocation and Sema will be destroyed.
      // Only SourceManager and FileManager remain.

      auto &SM = CI.getSourceManager();
      auto SrcBuffer = SM.getBufferData(SM.getMainFileID());

      auto SourceMD5 = levitation::calcMD5(SrcBuffer);
      auto OutputMD5 = levitation::calcMD5(LDepsBuffer);

      levitation::DeclASTMeta Meta(
        SourceMD5.Bytes,
        OutputMD5.Bytes,
        DeclASTMeta::FragmentsVectorTy()
      );

      assert(MetaOut.size());
      levitation::File F(MetaOut);

      if (auto OpenedFile = F.open()) {
        auto Writer = levitation::CreateMetaBitstreamWriter(OpenedFile.getOutputStream());
        Writer->writeAndFinalize(Meta);
      }

      if (F.hasErrors()) {
        CI.getDiagnostics().Report(
            diag::err_fe_levitation_dependency_file_failed_to_create_meta
        );
        return;
      }
    }

    void diagDependencyFileIOIssues(File::StatusEnum status) {
      auto &Diag = CI.getDiagnostics();
      switch (status) {
        case File::HasStreamErrors:
          Diag.Report(diag::err_fe_levitation_dependency_file_io_troubles);
          break;
        case File::FiledToRename:
        case File::FailedToCreateTempFile:
          Diag.Report(diag::err_fe_levitation_dependency_file_failed_to_create);
          break;
        default:
          break;
      }
    }
  };

  class LevitationUnitNamespaceVerifier : public SemaConsumer {
  private:
    Sema *SemaObj;
    bool HasMainFunction = false;
  public:
    LevitationUnitNamespaceVerifier(CompilerInstance &CI) {};

    void InitializeSema(Sema &S) override {
      SemaConsumer::InitializeSema(S);
      SemaObj = &S;
    }

    void ForgetSema() override {
      SemaConsumer::ForgetSema();
      SemaObj = nullptr;
    }

    void HandleTranslationUnit(ASTContext &Ctx) override {
      ASTConsumer::HandleTranslationUnit(Ctx);
      if (
        !HasMainFunction &&
        !SemaObj->levitationUnitScopeNotEmpty()
      )
        Ctx.getDiagnostics().Report(diag::warn_levitation_unit_decls);
    }

    bool HandleTopLevelDecl(DeclGroupRef D) override {

      // In general we can deal with group of declarations
      // For example typedef struct {} something_t;
      // So don't be surprised we're not using getSingleDecl here.

      for (const auto *d : D) {
        if (const auto *FD = dyn_cast<FunctionDecl>(d)) {
          if (FD->isMain())
            HasMainFunction = true;
        }
      }
      return ASTConsumer::HandleTopLevelDecl(D);
    }
  };

std::unique_ptr<LevitationPreprocessorConsumer> CreateDependenciesASTProcessor(
    CompilerInstance &CI
) {
  if (CI.getFrontendOpts().LevitationDependenciesOutputFile.empty())
    return nullptr;

  return std::make_unique<ASTDependenciesProcessor>(CI);
}

std::unique_ptr<ASTConsumer> CreateUnitNamespaceVerifier(
    CompilerInstance &CI
) {
  return std::make_unique<LevitationUnitNamespaceVerifier>(CI);
}

}}
