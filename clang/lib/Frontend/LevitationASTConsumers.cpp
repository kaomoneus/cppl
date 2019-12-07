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
#include "clang/Levitation/DeclASTMeta/DeclASTMeta.h"
#include "clang/Levitation/Dependencies.h"
#include "clang/Levitation/FileExtensions.h"
#include "clang/Levitation/Serialization.h"
#include "llvm/Bitstream/BitstreamWriter.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::levitation;

namespace {

  class SemaObjHolderConsumer : public SemaConsumer {
  protected:
    Sema *SemaObj;
  public:
    void InitializeSema(Sema &S) override {
      SemaObj = &S;
    }

    void ForgetSema() override {
      SemaObj = nullptr;
    }
  };

  class DependenciesValidator {
      StringRef SourcesRoot;
      StringRef FileExtension;
      FileManager *FileMgr;
      DiagnosticsEngine &Diag;
  public:
    DependenciesValidator(
      StringRef sourcesRoot,
      StringRef fileExtension,
      FileManager *fileMgr,
      DiagnosticsEngine &diag
    ) : SourcesRoot(sourcesRoot),
        FileExtension(fileExtension),
        FileMgr(fileMgr),
        Diag(diag)
    {}

    ValidatedDependenciesMap validate(const DependenciesMap& Dependencies) {
      ValidatedDependenciesMap ValidatedDependencies;
      for (const auto &Dep : Dependencies) {
        validate(ValidatedDependencies, Dep.second);
      }
      return ValidatedDependencies;
    }
  private:

    void validate(
            ValidatedDependenciesMap &Map,
            const PackageDependency &Dep
    ) {
      DependencyPath Path;

      DependencyComponentsVector ValidatedComponents;

      auto UnvalidatedComponents = Dep.getComponents();

      bool validated = buildPath(Path, ValidatedComponents, UnvalidatedComponents);

      if (!validated) {
        Map.setHasMissingDependencies();
        // FIXME Levitation: add diags
        return;
      }

      PackageDependency Validated(std::move(ValidatedComponents));
      Validated.setImportLoc(Dep.getImportLoc());
      Validated.setPath(std::move(Path));

      Map.mergeDependency(std::move(Validated));
    }

    bool buildPath(
        DependencyPath &Path,
        DependencyComponentsVector &ValidatedComponents,
        DependencyComponentsArrRef &UnvalidatedComponents
    ) {
      bool Valid = false;

      SmallString<256> FullPath = SourcesRoot;

      for (size_t i = 0, e = UnvalidatedComponents.size(); i != e; ++i) {

        const StringRef &Component = UnvalidatedComponents[i];
        ValidatedComponents.push_back(Component);

        llvm::sys::path::append(Path, Component);
        llvm::sys::path::append(FullPath, Component);

        if (isDirectory(FullPath))
          continue;

        if (FileExtension[0] != '.') {
          Path.push_back('.');
          FullPath.push_back('.');
        }

        Path.append(FileExtension.begin(), FileExtension.end());
        FullPath.append(FileExtension.begin(), FileExtension.end());

        if (isFile(FullPath))
          Valid = true;

        break;
      }
      return Valid;
    }

    bool isDirectory(const SmallVectorImpl<char> &Dir) {
      return (bool)FileMgr->getDirectory(StringRef(Dir.begin(), Dir.size()));
    }

    bool isFile(const SmallVectorImpl<char> &FName) {
      return (bool)FileMgr->getFile(StringRef(FName.begin(), FName.size()));
    }

    // TODO Levitation: will be used when we introduce manual
    //  dependency import.
    void diagMissingDependency(const levitation::PackageDependency &Dep) {
      // FIXME Levitation: SourceRange is not printed correctly.
      Diag.Report(
          Dep.getImportLoc().getBegin(),
          diag::err_levitation_dependency_missed
      ) << Dep << Dep.getImportLoc();
    }
  };

  class LevitationInputFileProcessor : public SemaObjHolderConsumer {
  protected:
    CompilerInstance &CI;
    DependencyPath CurrentInputFileRel;

    LevitationInputFileProcessor(CompilerInstance &ci, DependencyPath&& currentInputFileRel)
      : CI(ci),
        CurrentInputFileRel(std::move(currentInputFileRel))
    {
      if (llvm::sys::path::is_absolute(CurrentInputFileRel)) {
        llvm::errs()
        << "Invalid path:\n"
        << "    " << CurrentInputFileRel.str() << "\n";
        llvm_unreachable("Input file path should be relative (to source dir parameter)");
      }
    }
  };

  class DeclASTMetaCreator {
    const Sema* SemaObj;
    StringRef DeclASTBuffer;
  public:
    DeclASTMetaCreator(
        const Sema *SemaObj,
        StringRef DeclASTBuffer
    )
    : SemaObj(SemaObj),
      DeclASTBuffer(DeclASTBuffer) {}

    bool Write(StringRef OutputFile) {

      const auto &SM = SemaObj->getSourceManager();

      auto SourceMD5 = calcMD5(SM.getBufferData(SM.getMainFileID()));
      auto DeclASTMD5 = calcMD5(DeclASTBuffer);

      levitation::DeclASTMeta Meta(
        SourceMD5.Bytes,
        DeclASTMD5.Bytes,
        SemaObj->levitationGetSkippedBytes()
      );

      File F(OutputFile);

      if (auto OpenedFile = F.open()) {
        auto Writer = CreateMetaBitstreamWriter(OpenedFile.getOutputStream());
        Writer->writeAndFinalize(Meta);
      }

      if (F.hasErrors()) {
        diagMetaFileIOIssues(F.getStatus());
        return false;
      }
      return true;
    }
  private:

    template<typename BuffT>
    static MD5::MD5Result calcMD5(BuffT Buff) {
      MD5 Md5Builder;
      Md5Builder.update(Buff);
      MD5::MD5Result Result;
      Md5Builder.final(Result);
      return Result;
    }

    void diagMetaFileIOIssues(File::StatusEnum status) {
      auto &Diag = SemaObj->getDiagnostics();
      switch (status) {
        case File::HasStreamErrors:
          Diag.Report(diag::err_fe_levitation_decl_ast_meta_file_io_troubles);
          break;
        case File::FiledToRename:
        case File::FailedToCreateTempFile:
          Diag.Report(diag::err_fe_levitation_decl_ast_meta_failed_to_create);
          break;
        default:
          break;
      }
    }
  };

  class ASTDependenciesProcessor : public LevitationInputFileProcessor {
  public:
    ASTDependenciesProcessor(
        CompilerInstance &ci, DependencyPath&& currentInputFileRel
    )
    : LevitationInputFileProcessor(ci, std::move(currentInputFileRel)) {}

    void HandleTranslationUnit(ASTContext &Context) override {

      ValidatedDependenciesMap ValidatedDeclarationDependencies;
      ValidatedDependenciesMap ValidatedDefinitionDependencies;

      DependenciesValidator Validator(
          CI.getFrontendOpts().LevitationSourcesRootDir,
          levitation::FileExtensions::SourceCode,
          &CI.getFileManager(),
          CI.getDiagnostics()
      );

      PackageDependencies Dependencies {
        Validator.validate(SemaObj->getLevitationDeclarationDependencies()),
        Validator.validate(SemaObj->getLevitationDefinitionDependencies()),
        CurrentInputFileRel,
        SemaObj->isLevitationFilePublic()
      };

      auto F = createFile();

      if (auto OpenedFile = F.open()) {
        buffer_ostream buffer(OpenedFile.getOutputStream());
        auto Writer = CreateBitstreamWriter(buffer);
        Writer->writeAndFinalize(Dependencies);

        DeclASTMetaCreator MetaWriter(SemaObj, buffer.str());
        MetaWriter.Write(CI.getFrontendOpts().LevitationDeclASTMeta);
      }

      if (F.hasErrors()) {
        diagDependencyFileIOIssues(F.getStatus());
      }
    }
  private:
    File createFile() {
      return File(CI.getFrontendOpts().LevitationDependenciesOutputFile);
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
} // end anonymous namespace

namespace clang {

std::unique_ptr<ASTConsumer> CreateDependenciesASTProcessor(
    CompilerInstance &CI,
    StringRef InFile
) {
  if (CI.getFrontendOpts().LevitationDependenciesOutputFile.empty())
    return nullptr;

  auto InFileRel = levitation::Path::makeRelative<DependencyPath>(
      InFile,
      CI.getFrontendOpts().LevitationSourcesRootDir
  );

  return std::make_unique<ASTDependenciesProcessor>(CI, std::move(InFileRel));
}

}
