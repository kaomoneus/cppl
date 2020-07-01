//===--- Driver.h - C++ Driver class ----------------------------*- C++ -*-===//
//
// Part of the C++ Levitation Project,
// under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines C++ Levitation Driver class.
//  It is a public driver interface. Most of implementation is present
//  in .cpp file as separate classes.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LEVITATION_DRIVER_H
#define LLVM_LEVITATION_DRIVER_H

#include "clang/Levitation/Common/Path.h"
#include "clang/Levitation/Common/StringOrRef.h"
#include "clang/Levitation/Driver/DriverDefaults.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

namespace clang { namespace levitation { namespace log {
  class Logger;
}}}

namespace clang { namespace levitation { namespace tools {

  class LevitationDriver {
  public:
    using Args = llvm::SmallVector<StringOrRef, 8>;
  private:

    enum VerboseLevel {
      VerboseLevel0, // No verbose messages
      VerboseLevel1, // Only verbose messages
      VerboseLevel2, // Trace messages
    };

    VerboseLevel Verbose = VerboseLevel0;

    levitation::SinglePath BinDir;
    llvm::StringRef SourcesRoot = DriverDefaults::SOURCES_ROOT;
    llvm::SmallVector<llvm::StringRef, 16> LevitationLibs;
    llvm::StringRef BuildRoot = DriverDefaults::BUILD_ROOT;
    StringRef LibsOutSubDir = DriverDefaults::LIBS_OUTPUT_SUBDIR;
    llvm::StringRef PreambleSource;
    levitation::SinglePath PreambleOutput;
    levitation::SinglePath PreambleOutputMeta;

    int JobsNumber = DriverDefaults::JOBS_NUMBER;

    bool OutputHeadersDirDefault = true;
    levitation::SinglePath OutputHeadersDir;

    bool OutputDeclsDirDefault = true;
    levitation::SinglePath OutputDeclsDir;

    llvm::StringRef Output;

    bool LinkPhaseEnabled = true;

    bool DryRun = false;

    llvm::StringRef StdLib = DriverDefaults::STDLIB;
    bool CanUseLibStdCppForLinker = true;

    Args ExtraPreambleArgs;
    Args ExtraParseArgs;
    Args ExtraParseImportArgs;
    Args ExtraCodeGenArgs;
    Args ExtraLinkerArgs;

  public:

    LevitationDriver(llvm::StringRef CommandPath);

    bool isVerbose() const {
      return Verbose > VerboseLevel0;
    }

    void setVerbose() {
      Verbose = VerboseLevel1;
    }

    void setTrace() {
      Verbose = VerboseLevel2;
    }

    llvm::StringRef getSourcesRoot() const {
      return SourcesRoot;
    }

    void setSourcesRoot(llvm::StringRef SourcesRoot) {
      LevitationDriver::SourcesRoot = SourcesRoot;
    }

    void setBuildRoot(llvm::StringRef BuildRoot) {
      LevitationDriver::BuildRoot = BuildRoot;
      if (OutputHeadersDirDefault)
        OutputHeadersDir = levitation::Path::getPath<SinglePath>(
            BuildRoot, DriverDefaults::HEADER_DIR_SUFFIX
        );

      if (OutputDeclsDirDefault)
        OutputDeclsDir = levitation::Path::getPath<SinglePath>(
            BuildRoot, DriverDefaults::DECLS_DIR_SUFFIX
        );
    }

    StringRef getBuildRoot() const {
      return BuildRoot;
    }

    llvm::StringRef getPreambleSource() const {
      return PreambleSource;
    }

    bool isPreambleCompilationRequested() const {
      return PreambleSource.size();
    }

    void setPreambleSource(llvm::StringRef PreambleSource) {
      LevitationDriver::PreambleSource = PreambleSource;
    }

    void setStdLib(llvm::StringRef StdLib) {
      LevitationDriver::StdLib = StdLib;
    }

    void addLevitationLibPath(llvm::StringRef Path) {
      LevitationLibs.push_back(Path);
    }

    int getJobsNumber() const {
      return JobsNumber;
    }

    void setJobsNumber(int JobsNumber) {
      LevitationDriver::JobsNumber = JobsNumber;
    }

    llvm::StringRef getOutput() const {
      return Output;
    }

    void setOutput(llvm::StringRef Output) {
      LevitationDriver::Output = Output;
    }

    void setOutputHeadersDir(llvm::StringRef h) {
      OutputHeadersDir = h;
      OutputHeadersDirDefault = false;
    }

    void setOutputDeclsDir(llvm::StringRef h) {
      OutputDeclsDir = h;
      OutputDeclsDirDefault = false;
    }

    llvm::StringRef getOutputHeadersDir() const {
      return OutputHeadersDir;
    }

    llvm::StringRef getOutputDeclsDir() const {
      return OutputDeclsDir;
    }

    llvm::StringRef getLevitationLibrariesSubDir() const {
      return LibsOutSubDir;
    }

    bool shouldCreateHeaders() const {
      return !LinkPhaseEnabled;
    }

    bool shouldCreateDecls() const {
      return !LinkPhaseEnabled;
    }

    bool isLinkPhaseEnabled() const {
      return LinkPhaseEnabled;
    }

    void disableLinkPhase() {
      LinkPhaseEnabled = false;
    }

    bool isDryRun() const {
      return DryRun;
    }

    void setDryRun() {
      DryRun = true;
    }

    void disableUseLibStdCppForLinker() {
      LevitationDriver::CanUseLibStdCppForLinker = false;
    }

    void setExtraPreambleArgs(StringRef Args);
    void setExtraParserArgs(StringRef Args);
    void setExtraCodeGenArgs(StringRef Args);
    void setExtraLinkerArgs(StringRef Args);

    bool run();

    friend class LevitationDriverImpl;

  protected:

    void initParameters();
    void dumpParameters();
    void dumpExtraFlags(llvm::raw_ostream& Out, StringRef Phase, const Args &args);
  };
}}}

#endif //LLVM_LEVITATION_DRIVER_H
